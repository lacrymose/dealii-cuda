#ifndef _POISSON_COMMON_H
#define _POISSON_COMMON_H

#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria.h>
#include <deal.II/grid/manifold_lib.h>
#include <deal.II/base/vectorization.h>
#include <deal.II/base/function.h>
#include <deal.II/base/point.h>
#include "matrix_free_gpu/maybecuda.h"
#include "matrix_free_gpu/gpu_array.cuh"

// using namespace dealii;

//=============================================================================
// mesh creation and refinement
//=============================================================================


enum grid_case_t { UNIFORM, NONUNIFORM, RANDOM};
enum domain_case_t { CUBE, BALL, };

template <int dim>
bool all_criterion(const dealii::Point<dim> &p) {
  return true;
}

template <int dim>
bool octant_criterion(const dealii::Point<dim> &p) {
  bool ref = true;
  for(int d=0; d<dim; d++)
    ref = ref && p[d] > 0.2;
  return ref;
}

template <int dim>
bool random_criterion(const dealii::Point<dim> &p) {
  double r = (double)rand() / RAND_MAX;
  return r<0.5;
}


template <int dim>
void mark_cells(dealii::Triangulation<dim> &triangulation,
                bool (*crit)(const dealii::Point<dim> &))
{
  typename dealii::Triangulation<dim>::active_cell_iterator
    it = triangulation.begin_active(),
    end = triangulation.end();
  for(; it != end; ++it) {

    if(crit(it->center()))
      it->set_refine_flag();
  }
}

template <int dim>
void create_domain(dealii::Triangulation<dim> &triangulation,
                   domain_case_t domain)
{
  if(domain == CUBE) {
    dealii::GridGenerator::hyper_cube (triangulation, -1., 1.);
  }
  else if(domain == BALL) {
    dealii::GridGenerator::hyper_ball (triangulation);
    static const dealii::SphericalManifold<dim> boundary;
    triangulation.set_all_manifold_ids_on_boundary(0);
    triangulation.set_manifold (0, boundary);
  }

}

template <int dim>
void refine_mesh(dealii::Triangulation<dim> &triangulation,
                 grid_case_t grid_refinement)
{
  if(grid_refinement == UNIFORM)
    mark_cells(triangulation,all_criterion<dim>);
  else if(grid_refinement == NONUNIFORM)
    mark_cells(triangulation,octant_criterion<dim>);
  else if(grid_refinement == RANDOM)
    mark_cells(triangulation,random_criterion<dim>);

  triangulation.execute_coarsening_and_refinement();
}


template <int dim>
void create_mesh(dealii::Triangulation<dim> &triangulation,
                 domain_case_t domain,
                 grid_case_t refinement)
{
  create_domain(triangulation, domain);

  if(domain == CUBE)
    triangulation.refine_global ();

  triangulation.refine_global (3-dim);

  refine_mesh(triangulation,refinement);

}


//=============================================================================
// reference solution and right-hand side
//=============================================================================

template <int dim>
class Solution : public dealii::Function<dim>
{
private:
  static const unsigned int n_source_centers = 3;
  static const dealii::Point<dim>   source_centers[n_source_centers];
  static const double       width;

public:
  Solution () : dealii::Function<dim>() {}

  virtual double value (const dealii::Point<dim>   &p,
                        const unsigned int  component = 0) const;

  virtual dealii::Tensor<1,dim> gradient (const dealii::Point<dim>   &p,
                                  const unsigned int  component = 0) const;

  virtual double laplacian (const dealii::Point<dim>   &p,
                            const unsigned int  component = 0) const;
};


//=============================================================================
// coefficient
//=============================================================================

template <int dim>
struct Coefficient
{
  static MAYBECUDA_HOST double value (const dealii::Point<dim> &p){
    return 1. / (0.05 + 2.*(p.norm_square()));
    // return 1.;
  }

#ifdef MAYBECUDA_GUARD
  template <typename Number>
  static __device__ Number value (const GpuArray<dim,Number> &p){
    return 1. / (0.05 + 2.*(p.norm_square()));
    // return 1.;
  }
#endif

  template <typename Number>
  static dealii::VectorizedArray<Number> value (const dealii::Point<dim,dealii::VectorizedArray<Number>> &p){
    return 1. / (0.05 + 2.*(p.norm_square()));
    // dealii::VectorizedArray<Number> a;
    // a = 1.0;
    // return a;
  }


  static MAYBECUDA_HOST  dealii::Tensor<1,dim> gradient (const dealii::Point<dim> &p){
    const dealii::Tensor<1,dim> dist = -p;
    const double den = 0.05 + 2.*dist.norm_square();
    return (4. / (den*den))*dist;
    // const dealii::Tensor<1,dim> dist = p*0;
    // return dist;
  }
};

// Wrapper for coefficient
template <int dim>
class CoefficientFun : dealii::Function<dim>
{
public:
  CoefficientFun () : dealii::Function<dim>() {}

  virtual double value (const dealii::Point<dim>   &p,
                        const unsigned int  component = 0) const;

  virtual void value_list (const std::vector<dealii::Point<dim> > &points,
                           std::vector<double>            &values,
                           const unsigned int              component = 0) const;

  virtual dealii::Tensor<1,dim> gradient (const dealii::Point<dim>   &p,
                                  const unsigned int  component = 0) const;
};

template <int dim>
double CoefficientFun<dim>::value (const dealii::Point<dim>   &p,
                                   const unsigned int) const
{
  return Coefficient<dim>::value(p);
}

template <int dim>
void CoefficientFun<dim>::value_list (const std::vector<dealii::Point<dim> > &points,
                                      std::vector<double>            &values,
                                      const unsigned int              component) const
{
  Assert (values.size() == points.size(),
          ExcDimensionMismatch (values.size(), points.size()));
  Assert (component == 0,
          ExcIndexRange (component, 0, 1));

  const unsigned int n_points = points.size();
  for (unsigned int i=0; i<n_points; ++i)
    values[i] = value(points[i],component);
}


template <int dim>
dealii::Tensor<1,dim> CoefficientFun<dim>::gradient (const dealii::Point<dim>   &p,
                                             const unsigned int) const
{
  return Coefficient<dim>::gradient(p);
}


// function computing the right-hand side
template <int dim>
class RightHandSide : public dealii::Function<dim>
{
private:
  Solution<dim> solution;
  CoefficientFun<dim> coefficient;
public:
  RightHandSide () : dealii::Function<dim>() {}

  virtual double value (const dealii::Point<dim>   &p,
                        const unsigned int  component = 0) const;
};


template <int dim>
double RightHandSide<dim>::value (const dealii::Point<dim>   &p,
                                  const unsigned int) const
{
  return -(solution.laplacian(p)*coefficient.value(p)
           + coefficient.gradient(p)*solution.gradient(p));
}

#endif /* _POISSON_COMMON_H */
