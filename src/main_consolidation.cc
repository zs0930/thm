#include <deal.II/base/function.h>
#include <deal.II/base/function_lib.h>
#include <deal.II/base/logstream.h>
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/symmetric_tensor.h>
#include <deal.II/base/tensor_function.h>
#include <deal.II/base/utilities.h>

#include <deal.II/lac/affine_constraints.h>
#include <deal.II/lac/block_vector.h>
#include <deal.II/lac/block_sparse_matrix.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/solver_gmres.h>
#include <deal.II/lac/precondition.h>
#include <deal.II/lac/vector.h>
#include <deal.II/lac/solver_bicgstab.h>

#include <deal.II/grid/tria.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>
#include <deal.II/grid/grid_tools.h>
#include <deal.II/grid/grid_refinement.h>

#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_renumbering.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/fe_dgq.h>
#include <deal.II/fe/fe_raviart_thomas.h>
#include <deal.II/fe/fe_system.h>
#include <deal.II/fe/fe_values.h>

#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/matrix_tools.h>
#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/error_estimator.h>

#include <deal.II/lac/sparse_direct.h>
#include <deal.II/base/timer.h>


#include <cmath>
#include <fstream>
#include <iostream>

#include "json.hpp"

using namespace dealii;
using Json = nlohmann::json;

template <int dim>
class THMConsolidation
{
public:
  THMConsolidation(const unsigned degree, const nlohmann::json json);
  void run();

private:
  void make_grid();
  void setup_system();
  void assemble_system();
  void solve(unsigned step);
  void output_results(unsigned int step, unsigned int current_time) const;

  Triangulation<dim>        triangulation_;
  FESystem<dim>             fe_;
  DoFHandler<dim>           dof_handler_;
  //AffineConstraints<double> constraints_;

/*
  BlockSparsityPattern      sparsity_pattern_;
  BlockSparseMatrix<double> system_matrix_;
  BlockVector<double> solution_;
  BlockVector<double> system_rhs_;
*/
  SparsityPattern      sparsity_pattern_;
  SparseMatrix<double> system_matrix_;
  SparseMatrix<double> system_matrix_k_;
  SparseMatrix<double> system_matrix_c_;
  Vector<double> solution_;
  Vector<double> old_solution_;
  Vector<double> system_rhs_;
  Vector<double> solution_var_;

  unsigned         degree_;
  double           model_length_;
  double           height_over_width_;
  unsigned         subdivision_;
  unsigned         max_steps_;
  double           time_step_;
  double           current_time_;
  double           k_in_;
  double           lambda_in_;
  double           mu_in_;
  double           pressure_in_;
  double           step_torr_;
  double           first_step_;
  const double     gamma_w=1e5;
};

// For Dirichlet boundary conditions
template <int dim>
class DeformationDirichletBoundary : public Function<dim>
{
public:
  DeformationDirichletBoundary() : Function<dim>(dim + 1) {}
  virtual void vector_value(const Point<dim> &p, Vector<double> &  value) const override;
  void get_model_length_and_ratio(const double length, const double ratio) {model_length_ = length; height_over_width_ = ratio;}
private:
  double model_length_;
  double height_over_width_;
};

template <int dim>
void DeformationDirichletBoundary<dim>::vector_value(const Point<dim> &p, Vector<double> &  values) const
{
  if (p[2] == 0) {
    for (unsigned int c = 0; c < 3; ++c) {
      if (c==2){
        values(c) = 0;
      }
      else {
        values(c) = 0;
      }
      
    }
    //values(0) = 11; values(1) = 21; values(2) = 51; 
  } else if (p[0] == 0 || p[1] == 0 || std::abs(p[0]-model_length_) < 1e-12 || std::abs(p[1]-model_length_) < 1e-12) {
    for (unsigned int c = 0; c < 2; ++c) values(c) = 0;
    //values(0) = 31; values(1) = 41;
  }
}

template <int dim>
class PressureDirichletBoundary : public Function<dim>
{
public:
  PressureDirichletBoundary() : Function<dim>(dim + 1) {}
  virtual void vector_value(const Point<dim> &p, Vector<double> &  value) const override;
  void get_model_length_and_ratio(const double length, const double ratio) {model_length_ = length; height_over_width_ = ratio;}
private:
  double model_length_;
  double height_over_width_;
};

template <int dim>
void PressureDirichletBoundary<dim>::vector_value(const Point<dim> &p, Vector<double> &  values) const
{
  if (std::abs(p[2] - model_length_*height_over_width_) < 1e-12) values(3) = 0;
  // if (std::abs(p[2] - 0.0) < 1e-12) values(3) = 0.1;
}

template <int dim>
class DeformationNeumannBoundary : public TensorFunction<1,dim>
{
public:
  DeformationNeumannBoundary() : TensorFunction<1,dim>() {}
  virtual void value_list(const std::vector<Point<dim>> &points, 
                          std::vector<Tensor<1,dim>> & values) const override;
  void get_pressure(const double pressure) {pressure_ = pressure; }
private:
  double pressure_;
};

template <int dim>
void DeformationNeumannBoundary<dim>::value_list(const std::vector<Point<dim>> &points, 
                                                 std::vector<Tensor<1,dim>> & values) const
{
  (void)points;
  AssertDimension(points.size(), values.size());
  for(auto &value : values) {
    value[0] = 0; value[1] = 0; value[2] = -pressure_;
  }
}

// For initial conditions
template <int dim>
class DeformationInitial : public Function<dim>
{
public:
  DeformationInitial() : Function<dim>(dim + 1) {}
  virtual void vector_value(const Point<dim> &p, Vector<double> &  value) const override;
};

template <int dim>
void DeformationInitial<dim>::vector_value(const Point<dim> &p, Vector<double> &  values) const
{
  (void)p;
  for (unsigned int c = 0; c < this->n_components; ++c)
    values(c) = 0.0;
}

template <int dim>
class PressureInitial : public Function<dim>
{
public:
  PressureInitial() : Function<dim>(dim + 1) {}
  virtual void vector_value(const Point<dim> &p, Vector<double> &  value) const override;
  void get_pressure(const double pressure) {pressure_ = pressure; }
private:
  double pressure_;
};

template <int dim>
void PressureInitial<dim>::vector_value(const Point<dim> &p, Vector<double> &  values) const
{
  (void)p;
  values(3) = pressure_;
}



template <int dim>
THMConsolidation<dim>::THMConsolidation(const unsigned degree, const nlohmann::json json)
  : degree_(degree)
  , fe_(FE_Q<dim>(degree+1), dim, FE_Q<dim>(degree), 1)
  , dof_handler_(triangulation_)
{
  model_length_ = json["parameters"]["model_length"].template get<double>();
  height_over_width_ = json["parameters"]["height_over_width"].template get<double>();
  subdivision_ = json["parameters"]["subdivision"].template get<unsigned>();
  max_steps_ = json["parameters"]["max_steps"].template get<unsigned>();
  time_step_ = json["parameters"]["time_step"].template get<double>();
  pressure_in_ = json["parameters"]["loading_on_top"].template get<double>();
  k_in_ = json["parameters"]["k"].template get<double>();
  lambda_in_ = json["parameters"]["lambda"].template get<double>();
  mu_in_ = json["parameters"]["mu"].template get<double>();
  std::cout<<"Model length: "<<model_length_<<std::endl<<"height/width: "<<height_over_width_<<std::endl<<"Subdivision: "<<subdivision_<<std::endl
           <<"Total steps: "<<max_steps_<<std::endl<<"Time step: "<<time_step_<<std::endl
           <<"k: "<<k_in_<<", lambda: "<<lambda_in_<<", mu: "<<mu_in_<<std::endl;
}

template <int dim>
void THMConsolidation<dim>::make_grid()
{
  //Tensor<1,dim> TP1;
  //TP1(0)=0;TP1(1)=0;TP1(2)=0;
  //Tensor<1,dim> TP2;
  //TP2(0)=model_length_;TP2(1)=model_length_;TP2(2)=model_length_;
  const Point<dim,double> grid_p1(0.0, 0.0, 0.0);
  const Point<dim,double> grid_p2(model_length_, model_length_, model_length_ * height_over_width_);
  const std::vector<unsigned int> repetitions{subdivision_, subdivision_, subdivision_ * height_over_width_};
  GridGenerator::subdivided_hyper_rectangle(triangulation_, repetitions, grid_p1, grid_p2);

  // Set boundary indicator for applying boundary conditions
  // -x:1, +x:2, -y:3, +y:4, -z:5, +z:6 
  if (dim == 3) {
    for (const auto &cell : triangulation_.active_cell_iterators()) {
      for (const auto &face : cell->face_iterators()) {
        if (face->center()[0] == 0) face->set_all_boundary_ids(1);
        else if (std::abs(face->center()[0] - model_length_) < 1e-12) face->set_all_boundary_ids(2);
      }
    }
    for (const auto &cell : triangulation_.active_cell_iterators()) {
      for (const auto &face : cell->face_iterators()) {
        if (face->center()[1] == 0) face->set_all_boundary_ids(3);
        else if (std::abs(face->center()[1] - model_length_) < 1e-12) face->set_all_boundary_ids(4);
      }
    }
    for (const auto &cell : triangulation_.active_cell_iterators()) {
      for (const auto &face : cell->face_iterators()) {
        if (face->center()[2] == 0) face->set_all_boundary_ids(5);
        else if (std::abs(face->center()[2] - model_length_*height_over_width_) < 1e-12) face->set_all_boundary_ids(6);
      }
    }
  }

  dof_handler_.distribute_dofs(fe_);

  // Re-numner dof, make velocities and pressures are not intermingled
  DoFRenumbering::component_wise(dof_handler_);

/*
  // Set constraints to apply Dirichlet boundary conditions
  {
    constraints_.clear();

    DeformationDirichletBoundary<dim> deformation_dirichlet;
    deformation_dirichlet.get_model_length_and_ratio(model_length_, height_over_width_);
    PressureDirichletBoundary<dim> pressure_dirichlet;
    pressure_dirichlet.get_model_length_and_ratio(model_length_, height_over_width_);

    FEValuesExtractors::Vector deformations_bottom(0);
    FEValuesExtractors::Scalar pressure(dim);
    std::vector<bool> deformations_side{true, true, false, false};
    DoFTools::make_hanging_node_constraints(dof_handler_, constraints_);
    for (int i = 1; i <= 4; ++i) {
      VectorTools::interpolate_boundary_values(dof_handler_,
                                               i,
                                               deformation_dirichlet,
                                               constraints_,
                                               fe_.component_mask(deformations_side));
    }
    VectorTools::interpolate_boundary_values(dof_handler_,
                                             5,
                                             deformation_dirichlet,
                                             constraints_,
                                             fe_.component_mask(deformations_bottom));
     VectorTools::interpolate_boundary_values(dof_handler_,
                                              6,
                                              pressure_dirichlet,
                                              constraints_,
                                              fe_.component_mask(pressure));
//
          // VectorTools::interpolate_boundary_values(dof_handler_,
          //                                      5,
          //                                      pressure_dirichlet,
          //                                      constraints_,
          //                                      fe_.component_mask(pressure));
                                              
  }
  constraints_.close();
*/


  // Count the number of velocity and pressure dofs
  const std::vector<types::global_dof_index> dofs_per_component =
    DoFTools::count_dofs_per_fe_component(dof_handler_);
  unsigned int n_u = 0;
  for(int i = 0; i < dim; ++i) n_u += dofs_per_component[i];
  const unsigned int n_p = dofs_per_component[dim];

  std::cout << "Number of active cells: " << triangulation_.n_active_cells()<< std::endl
            << "Total number of cells: " << triangulation_.n_cells()<< std::endl
            << "Number of degrees of freedom: " << dof_handler_.n_dofs()
            << " (" << n_u << '+' << n_p << ')' << std::endl;

  // Allocate sparsity patterns
/*
  BlockDynamicSparsityPattern dsp(2, 2);
  dsp.block(0, 0).reinit(n_u, n_u);
  dsp.block(1, 0).reinit(n_p, n_u);
  dsp.block(0, 1).reinit(n_u, n_p);
             +
  DoFTools::make_sparsity_pattern(dof_handler_, dsp, constraints_, false);

  sparsity_pattern_.copy_from(dsp);
  system_matrix_.reinit(sparsity_pattern_);

  // Resize the solution and right hand side vectors
  solution_.reinit(2);
  solution_.block(0).reinit(n_u);
  solution_.block(1).reinit(n_p);
  solution_.collect_sizes();
  system_rhs_.reinit(2);
  system_rhs_.block(0).reinit(n_u);
  system_rhs_.block(1).reinit(n_p);
  system_rhs_.collect_sizes();
*/
  DynamicSparsityPattern dsp(n_u+n_p, n_u+n_p);
  DoFTools::make_sparsity_pattern(dof_handler_, dsp/*, constraints_, false*/);
  sparsity_pattern_.copy_from(dsp);
  system_matrix_.reinit(sparsity_pattern_);
  system_matrix_=0;
  system_matrix_k_.reinit(sparsity_pattern_);
  system_matrix_k_=0;
  system_matrix_c_.reinit(sparsity_pattern_);
  system_matrix_c_=0;
  solution_.reinit(n_u+n_p);
  solution_ = 0;
  system_rhs_.reinit(n_u+n_p);
  system_rhs_=0;
}

// Assemble matrix system
template <int dim>
void THMConsolidation<dim>::assemble_system()
{
  const std::vector<size_t> deformation_idx{0, 1, 2};
  const std::vector<size_t> pressure_idx{3};

  system_matrix_ = 0;
  system_matrix_k_ = 0;
  system_matrix_c_ = 0;
  system_rhs_    = 0;

  QGauss<dim>     quadrature_formula(degree_+2 );
  QGauss<dim - 1> face_quadrature_formula(degree_+2 );

  FEValues<dim>     fe_values(fe_,
                          quadrature_formula,
                          update_values | update_gradients |
                            update_quadrature_points | update_JxW_values);
  FEFaceValues<dim> fe_face_values(fe_,
                                   face_quadrature_formula,
                                   update_values | update_normal_vectors |
                                     update_quadrature_points |
                                     update_JxW_values);

  const unsigned int dofs_per_cell = fe_.dofs_per_cell;

  const unsigned int n_q_points      = quadrature_formula.size();
  const unsigned int n_face_q_points = face_quadrature_formula.size();

  // Initialize local matrices
  FullMatrix<double> local_matrix(dofs_per_cell, dofs_per_cell);
  FullMatrix<double> local_matrix_k(dofs_per_cell, dofs_per_cell);
  FullMatrix<double> local_matrix_c(dofs_per_cell, dofs_per_cell);
  Vector<double>     local_rhs(dofs_per_cell);

  // Initialize local old solution containers
  std::vector<Vector<double>> old_solution_values(n_q_points,
                                                  Vector<double>(dim+1));
  //std::vector<Vector<double>> old_solution_values_face(n_face_q_points,
                                                       //Vector<double>(dim + 1));

  std::vector<types::global_dof_index> local_dof_indices(dofs_per_cell);

  // Initialize defined functions for boundary values
  DeformationNeumannBoundary<dim> deformation_neumann_boundary;
  deformation_neumann_boundary.get_pressure(pressure_in_);
  //PressureNeumannBoundary<dim> pressure_neumann_boundary;
  //GravityValues<dim> gravity_boundary_values;
  std::vector<double> lambda_values(n_q_points);
  std::vector<double> mu_values(n_q_points);
  std::vector<double> k_values(n_q_points);
  Functions::ConstantFunction<dim> lambda(lambda_in_), mu(mu_in_), k(k_in_);

  std::vector<Tensor<1, dim>>         deformation_neumann_boundary_values(n_face_q_points);
  //std::vector<Tensor<1, dim>> boundary_g_values(n_q_points);
  //std::vector<Tensor<1, dim>> pressure_neumann_boundary_values(n_q_points);
  std::vector<double>         boundary_k_values(n_face_q_points);

  const FEValuesExtractors::Vector deformations(0);
  const FEValuesExtractors::Scalar pressure(dim);

  for (const auto &cell : dof_handler_.active_cell_iterators()) {
    fe_values.reinit(cell);
    local_matrix = 0;
    local_matrix_k = 0;
    local_matrix_c = 0;
    local_rhs    = 0;

    lambda.value_list(fe_values.get_quadrature_points(), lambda_values);
    mu.value_list(fe_values.get_quadrature_points(), mu_values);
    k.value_list(fe_values.get_quadrature_points(), k_values);
    
    // Get old solution value at Gauss points
    fe_values.get_function_values(solution_, old_solution_values);

    for (unsigned int q = 0; q < n_q_points; ++q) {
      for (unsigned int i = 0; i < dofs_per_cell; ++i) {

        const unsigned int component_i = fe_.system_to_component_index(i).first;
        const Tensor<1, dim> phi_i_u = fe_values[deformations].value(i, q);
        //const Tensor<2, dim> grad_phi_i_u = fe_values[deformations].gradient(i, q);
        const SymmetricTensor<2,dim> symmgrad_phi_i_u = fe_values[deformations].symmetric_gradient(i, q);
        const double phi_i_p     = fe_values[pressure].value(i, q);
        const Tensor<1, dim> grad_phi_i_p = fe_values[pressure].gradient(i, q);
        const double div_phi_i_u=fe_values[deformations].divergence(i,q);

        for (unsigned int j = 0; j < dofs_per_cell; ++j) {

          const unsigned int component_j =  fe_.system_to_component_index(j).first;
          const Tensor<1, dim> phi_j_u = fe_values[deformations].value(j, q);
          //const Tensor<2, dim> grad_phi_j_u = fe_values[deformations].gradient(j, q);
          const SymmetricTensor<2,dim> symmgrad_phi_j_u = fe_values[deformations].symmetric_gradient(j, q);
          const double phi_j_p     = fe_values[pressure].value(j, q);
          const Tensor<1, dim> grad_phi_j_p = fe_values[pressure].gradient(j, q);
          const double div_phi_j_u=fe_values[deformations].divergence(j,q);

          Tensor<1, dim> sub_solution_deformation;
          double sub_solution_pressure;
          for(auto id : deformation_idx) sub_solution_deformation[id] = old_solution_values[q](id);
          sub_solution_pressure = old_solution_values[q](dim);
/*
          local_matrix(i, j) +=
             (
             (lambda_values[q] * div_phi_i_u * div_phi_j_u)                         
             +   
             (2 * mu_values[q] *symmgrad_phi_i_u * symmgrad_phi_j_u)
             +  
             (-div_phi_i_u*phi_j_p)
             +
             (grad_phi_i_p * k_values[q] * grad_phi_j_p)/gamma_w
             +
             ( phi_i_p*div_phi_j_u/(time_step_) )
             )*fe_values.JxW(q);
*/

          local_matrix_k(i, j) +=
             (
             (lambda_values[q] * div_phi_i_u * div_phi_j_u)                         
             +   
             (2 * mu_values[q] *symmgrad_phi_i_u * symmgrad_phi_j_u)
             +  
             (-div_phi_i_u*phi_j_p)
             +
             (grad_phi_i_p * k_values[q] * grad_phi_j_p)/gamma_w
             )*fe_values.JxW(q);

          local_matrix_c(i, j) +=
             (
             ( phi_i_p*div_phi_j_u/(time_step_/* std::pow(10,step-1)*/) )
             )*fe_values.JxW(q);
/*
          local_rhs(i) +=
           (
             phi_i_p * div_phi_j_u / (time_step_ )
           )
           *
            (
              phi_j_u * sub_solution_deformation +
              phi_j_p * sub_solution_pressure
            )
            * fe_values.JxW(q);
*/
        } // for j
        
      } // for i

        //std::cout<<"local rhs "<<local_rhs<<std::endl;

    } // for q



    // Apply Neumann boundary conditions
    for (const auto &face : cell->face_iterators()) {
      if (face->boundary_id() == 6) {

        fe_face_values.reinit(cell, face);

        deformation_neumann_boundary.value_list(fe_face_values.get_quadrature_points(), deformation_neumann_boundary_values);
        k.value_list(fe_face_values.get_quadrature_points(), boundary_k_values);
        //gravity_boundary_values.value_list(fe_face_values.get_quadrature_points(), boundary_g_values);

        for (unsigned int q = 0; q < n_face_q_points; ++q) {
          const Tensor<1, dim> nf = fe_face_values.normal_vector(q);

          for (unsigned int i = 0; i < dofs_per_cell; ++i) {
            const unsigned int component_i = fe_.system_to_component_index(i).first;
            const Tensor<1, dim> phi_i_u = fe_face_values[deformations].value(i, q);
            const double phi_i_p = fe_face_values[pressure].value(i, q);

            local_rhs(i) +=
             (
              (phi_i_u * deformation_neumann_boundary_values[q])
             ) * fe_face_values.JxW(q);   

          } // for i
        } // for q

      } // if face at boundary 6
    } // for face

    // Assemble local matrix into global
    cell->get_dof_indices(local_dof_indices);
    for (unsigned int i = 0; i < dofs_per_cell; ++i) {
      for (unsigned int j = 0; j < dofs_per_cell; ++j) {
        system_matrix_.add(local_dof_indices[i],
                           local_dof_indices[j],
                           local_matrix_k(i, j));
        system_matrix_.add(local_dof_indices[i],
                           local_dof_indices[j],
                           local_matrix_c(i, j));
        system_matrix_c_.add(local_dof_indices[i],
                           local_dof_indices[j],
                           local_matrix_c(i, j));
      }
    }
    for (unsigned int i = 0; i < dofs_per_cell; ++i)
      system_rhs_(local_dof_indices[i]) += local_rhs(i); 
    //constraints_.distribute_local_to_global(local_matrix, local_s, local_dof_indices, system_matrix_, system_rhs_);

  } // for cell

  // rhs += C/t * solution
  system_matrix_c_.vmult_add(system_rhs_, solution_);

  // Apply Dirichlet boundary conditions
  {
    DeformationDirichletBoundary<dim> deformation_dirichlet;
    deformation_dirichlet.get_model_length_and_ratio(model_length_, height_over_width_);
    PressureDirichletBoundary<dim> pressure_dirichlet;
    pressure_dirichlet.get_model_length_and_ratio(model_length_, height_over_width_);

    FEValuesExtractors::Vector deformations_bottom(0);
    FEValuesExtractors::Scalar pressure(dim);
    std::vector<bool> deformations_side{true, true, false, false};

    for (int i = 1; i <= 4; ++i) {
      std::map<types::global_dof_index, double> deformation_side_values;
      VectorTools::interpolate_boundary_values(dof_handler_,
                                               i,
                                               deformation_dirichlet,
                                               deformation_side_values,
                                               fe_.component_mask(deformations_side));
      MatrixTools::apply_boundary_values(deformation_side_values, system_matrix_, solution_, system_rhs_);
    }

    std::map<types::global_dof_index, double> deformation_bottom_values;
    VectorTools::interpolate_boundary_values(dof_handler_,
                                             5,
                                             deformation_dirichlet,
                                             deformation_bottom_values,
                                             fe_.component_mask(deformations_bottom));
    MatrixTools::apply_boundary_values(deformation_bottom_values, system_matrix_, solution_, system_rhs_);

    std::map<types::global_dof_index, double> pressure_values;
    VectorTools::interpolate_boundary_values(dof_handler_,
                                             6,
                                             pressure_dirichlet,
                                             pressure_values,
                                             fe_.component_mask(pressure));
    MatrixTools::apply_boundary_values(pressure_values, system_matrix_, solution_, system_rhs_);
  }

}


// GMRES solver
template <int dim>
void THMConsolidation<dim>::solve(unsigned step)
{

  //std::ofstream out("sparsity_pattern.svg");
  //sparsity_pattern_.print_svg(out);

  // std::ofstream out1("matrix.txt");
  // system_matrix_.print_as_numpy_arrays(out1);
  
if (step < 5)
{ std::cout<<system_rhs_.l2_norm()<<" l2norm "<<std::endl   ;    
  SolverControl               solver_control(std::max<std::size_t>(1000,
                                                      system_rhs_.size() / 10),
                                1e-7 * system_rhs_.l2_norm());
                       
  //  SolverGMRES<Vector<double>> solver(solver_control);
   SolverBicgstab<Vector<double>> solver(solver_control);
   PreconditionJacobi<SparseMatrix<double>> preconditioner;
   preconditioner.initialize(system_matrix_, 1.0);
   solver.solve(system_matrix_, solution_, system_rhs_, preconditioner);

   Vector<double> residual(dof_handler_.n_dofs());

   system_matrix_.vmult(residual, solution_);
   residual -= system_rhs_;
   std::cout << "   Iterations required for convergence: "
             << solver_control.last_step() << '\n'
             << "   Max norm of residual:                "
             << residual.linfty_norm() << '\n';
             }

else
{
    SparseDirectUMFPACK A_direct;
  A_direct.initialize(system_matrix_);
  //A_direct.vmult(solution_, system_rhs_);
  A_direct.factorize(system_matrix_);
  A_direct.solve(system_rhs_);
  solution_ = system_rhs_;
}
/*
  // Direct solver

*/
/*
    std::cout 
             << "   Max norm of rhs: "
             << system_rhs_.linfty_norm() << '\n' << std::endl;
    std::cout 
             << "   Max norm of matrix: "
             << system_matrix_.linfty_norm() << '\n' << std::endl;
    std::cout << "Solving linear system... ";
    Timer timer;
    SparseDirectUMFPACK A_direct;
    A_direct.initialize(system_matrix_);
    A_direct.vmult(solution_, system_rhs_);
    timer.stop();
    std::cout << "done (" << timer.cpu_time() << "s)" << std::endl;
    std::cout 
             << "   Max norm of solution: "
             << solution_.linfty_norm() << '\n' << std::endl;
    Vector<double> residual(dof_handler_.n_dofs());
    system_matrix_.vmult(residual, solution_);
    residual -= system_rhs_;
    std::cout 
             << "   Max norm of residual: "
             << residual.linfty_norm() << '\n' << std::endl;
*/
  //constraints_.distribute(solution_);
}


// Output results
template <int dim>
void THMConsolidation<dim>::output_results(unsigned int time_step, unsigned current_time) const
{
  std::vector<std::string> solution_names(dim, "deformation");
  solution_names.emplace_back("pressure");

  std::vector<DataComponentInterpretation::DataComponentInterpretation>
    data_component_interpretation(
      dim, DataComponentInterpretation::component_is_part_of_vector);
  data_component_interpretation.push_back(
    DataComponentInterpretation::component_is_scalar);

  DataOut<dim> data_out;
  data_out.attach_dof_handler(dof_handler_);
  data_out.add_data_vector(solution_,
                           solution_names,
                           DataOut<dim>::type_dof_data,
                           data_component_interpretation);
  data_out.build_patches();
  std::ofstream time_file ("time.txt",std::ios::app);
  double value       = current_time_;
    //myfile.write (*conversion, strlen (conversion));
  time_file <<value<<"\n";
  time_file.close();

  std::ofstream output(
    "outputfiles/solution-" + Utilities::int_to_string(time_step, 2) + ".vtk");
  data_out.write_vtk(output);
}

template <int dim>
void THMConsolidation<dim>::run()
{
  Timer timer_grid;
  make_grid();

  // Apply initial values
  { 
    std::vector<bool> deformations{true, true, true, false};
    std::vector<bool> pressure{false, false, false, true};

    PressureInitial<dim> pressure_initial;
    pressure_initial.get_pressure(pressure_in_);
    
    VectorTools::interpolate(dof_handler_, DeformationInitial<dim>(),
                             solution_, deformations);
    VectorTools::interpolate(dof_handler_, pressure_initial,
                             solution_, pressure);
  }
  output_results(0,0);
  timer_grid.stop();
  std::cout << "Initialize mesh and initial conditions: " << timer_grid.cpu_time() << "s" << std::endl;
  first_step_=gamma_w*(model_length_/subdivision_)*(model_length_/subdivision_)/(6*2*mu_in_*k_in_);
  step_torr_=1e-2*pressure_in_*std::sqrt(system_rhs_.size());
  current_time_=0;
  double err=pressure_in_*std::sqrt(system_rhs_.size())+1;
  old_solution_=0;
  for(unsigned step = 1; err>1e-5*pressure_in_*std::sqrt(system_rhs_.size()) ; ++step) {
    if (step==1) {
      time_step_=first_step_;
    }


    Timer timer_assembler;
    assemble_system();
    timer_assembler.stop();
    std::cout << "Step: "<<step<<", assembler: " << timer_assembler.cpu_time() << "s" << std::endl;
  
    Timer timer_solver;
    solve(step);
    timer_solver.stop();
    std::cout << "Step: "<<step<<", solver: " << timer_solver.cpu_time() << "s" << std::endl;
    solution_var_=old_solution_;
    solution_var_-=solution_;
    std::cout<<solution_var_.l2_norm()<<" norm"<<std::endl;
    std::cout<<time_step_<<" adaptive_time_step"<<std::endl;
    current_time_+=time_step_;
    std::cout<<current_time_<<" current_time"<<std::endl;
    old_solution_=solution_; 
    if (step>1){
      err=solution_var_.l2_norm();
    }
    // std::cout<<err<<" err"<<std::endl;
    // std::cout<<1e-2*pressure_in_*std::sqrt(system_rhs_.size())<<" err1"<<std::endl;
    


    Timer timer_out;
    output_results(step,current_time_);
    timer_out.stop();
    std::cout << "Step: "<<step<<", output vtk files: " << timer_out.cpu_time() << "s" << std::endl;
    if (err<step_torr_/2){
      time_step_*=5;
    }
    else if (err>step_torr_*2){
      time_step_/=5;
    }
  }

}


int main(int argc, char** argv) {

  try {

    if (argc != 2) {
      std::cout << "Usage: ./thm /path/to/input.json\n";
      throw std::runtime_error("Incorrect number of input arguments");
    }

    const std::string filename = argv[1];

    // Input file
    std::ifstream in(filename); 
    const nlohmann::json input_json = Json::parse(in);

    std::string title = input_json["title"].template get<std::string>();
    unsigned degree = input_json["degree"].template get<unsigned>();
    std::cout<<title<<", with degree: "<<degree<<std::endl;

    THMConsolidation<3> thm_consolidation_solver(degree, input_json);
    thm_consolidation_solver.run();

  } catch (std::exception& exc) {
    std::cerr << std::endl
              << std::endl
              << "----------------------------------------------------"
              << std::endl;
    std::cerr << "Exception on processing: " << std::endl
              << exc.what() << std::endl
              << "Aborting!" << std::endl
              << "----------------------------------------------------"
              << std::endl;
    return 1;
  } catch (...) {
    std::cerr << std::endl
              << std::endl
              << "----------------------------------------------------"
              << std::endl;
    std::cerr << "Unknown exception!" << std::endl
              << "Aborting!" << std::endl
              << "----------------------------------------------------"
              << std::endl;
    return 1;
  }
  return 0;

}
