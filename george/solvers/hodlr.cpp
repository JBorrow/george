#include <pybind11/pybind11.h>
#include <pybind11/eigen.h>
#include <Eigen/Core>

#include "george/hodlr.h"
#include "george/kernels.h"
#include "george/parser.h"
#include "george/exceptions.h"

namespace py = pybind11;

using RowMatrixXd = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

class SolverMatrix {
  public:
    SolverMatrix (george::kernels::Kernel* kernel)
      : kernel_(kernel) {};
    void set_input_coordinates (RowMatrixXd x) {
      if (size_t(x.cols()) != kernel_->get_ndim()) {
        throw george::dimension_mismatch();
      }
      t_ = x;
    };
    double get_value (const int i, const int j) {
      if (i < 0 || i >= t_.rows() || j < 0 || j >= t_.rows()) {
        throw std::out_of_range("attempting to index outside of the dimension of the input coordinates");
      }
      return kernel_->value(t_.row(i).data(), t_.row(j).data());
    };

  private:
    george::kernels::Kernel* kernel_;
    RowMatrixXd t_;
};

class Solver {
public:

  Solver () {
    solver_ = NULL;
    kernel_ = NULL;
    matrix_ = NULL;
    computed_ = 0;
  };
  Solver (py::object kernel_spec, int min_size = 100, double tol = 0.1, int seed = 0)
    : kernel_spec_(kernel_spec)
    , tol_(tol)
    , min_size_(min_size)
    , seed_(seed)
  {
    solver_ = NULL;
    kernel_ = george::parse_kernel_spec(kernel_spec);
    matrix_ = new SolverMatrix(kernel_);
    computed_ = 0;
  };
  ~Solver () {
    if (solver_ != NULL) delete solver_;
    if (matrix_ != NULL) delete matrix_;
    if (kernel_ != NULL) delete kernel_;
  };

  auto serialize () const {
    return std::make_tuple(kernel_spec_, min_size_, tol_, seed_);
  };

  void deserialize (py::object kernel_spec, int min_size, double tol, int seed) {
    kernel_spec_ = kernel_spec;
    min_size_ = min_size;
    tol_ = tol;
    seed_ = seed;

    solver_ = NULL;
    kernel_ = george::parse_kernel_spec(kernel_spec);
    matrix_ = new SolverMatrix(kernel_);
    computed_ = 0;
  };

  int get_status () const { return 0; };
  int get_computed () const { return computed_; };
  double log_determinant () const { return log_det_; };

  int compute (const py::array_t<double>& x, const py::array_t<double>& yerr) {
    computed_ = 0;

    // Random number generator for reproducibility
    std::random_device r;
    std::mt19937 random(r());
    random.seed(seed_);

    // Extract the data from the numpy arrays
    auto x_p = x.unchecked<2>();
    auto yerr_p = yerr.unchecked<1>();
    size_t n = x_p.shape(0), ndim = x_p.shape(1);
    RowMatrixXd X(n, ndim);
    Eigen::VectorXd diag(n);
    for (size_t i = 0; i < n; ++i) {
      diag(i) = yerr_p(i) * yerr_p(i);
      for (size_t j = 0; j < ndim; ++j) X(i, j) = x_p(i, j);
    }

    matrix_->set_input_coordinates(X);

    // Set up the solver object.
    if (solver_ != NULL) delete solver_;
    solver_ = new george::hodlr::Node<SolverMatrix> (
        diag, matrix_, 0, n, min_size_, tol_, random);
    solver_->compute();
    log_det_ = solver_->log_determinant();

    // Update the bookkeeping flags.
    computed_ = 1;
    size_ = n;
    return 0;
  };

  template <typename Derived>
  void apply_inverse (Eigen::MatrixBase<Derived>& x) {
    if (!computed_) throw george::not_computed();
    solver_->solve(x);
  };

  int size () const { return size_; };

private:
  py::object kernel_spec_;
  double log_det_, tol_;
  int min_size_, seed_, size_;
  int computed_;

  george::kernels::Kernel* kernel_;
  SolverMatrix* matrix_;
  george::hodlr::Node<SolverMatrix>* solver_;
};



PYBIND11_PLUGIN(hodlr) {
  py::module m("hodlr", R"delim(
Docs...
)delim");

  py::class_<Solver> solver(m, "HODLRSolver");
  solver.def(py::init<py::object, int, double, int>(),
      py::arg("kernel_spec"), py::arg("min_size") = 100, py::arg("tol") = 10, py::arg("seed") = 42);
  solver.def_property_readonly("computed", &Solver::get_computed);
  solver.def_property_readonly("log_determinant", &Solver::log_determinant);
  solver.def("compute", &Solver::compute);
  solver.def("apply_inverse", [](Solver& self, Eigen::MatrixXd& x, bool in_place = false){
    if (in_place) {
      self.apply_inverse(x);
      return x;
    }
    Eigen::MatrixXd alpha = x;
    self.apply_inverse(alpha);
    return alpha;
  }, py::arg("x"), py::arg("in_place") = false);

  solver.def("dot_solve", [](Solver& self, const Eigen::VectorXd& x){
    Eigen::VectorXd alpha = x;
    self.apply_inverse(alpha);
    return double(x.transpose() * alpha);
  });

  solver.def("get_inverse", [](Solver& self){
    int n = self.size();
    Eigen::MatrixXd eye(n, n);
    eye.setIdentity();
    self.apply_inverse(eye);
    return eye;
  });

  solver.def("__getstate__", [](const Solver& self) {
    return self.serialize();
  });

  solver.def("__setstate__", [](Solver& self, py::tuple t) {
    if (t.size() != 4) throw std::runtime_error("Invalid state!");
    new (&self) Solver();
    self.deserialize(
      t[0].cast<py::object>(),
      t[1].cast<int>(),
      t[2].cast<double>(),
      t[3].cast<int>()
    );
  });

  return m.ptr();
}
