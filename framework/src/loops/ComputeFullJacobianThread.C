//* This file is part of the MOOSE framework
//* https://www.mooseframework.org
//*
//* All rights reserved, see COPYRIGHT for full restrictions
//* https://github.com/idaholab/moose/blob/master/COPYRIGHT
//*
//* Licensed under LGPL 2.1, please see LICENSE for details
//* https://www.gnu.org/licenses/lgpl-2.1.html

#include "ComputeFullJacobianThread.h"
#include "NonlinearSystem.h"
#include "FEProblem.h"
#include "KernelBase.h"
#include "IntegratedBCBase.h"
#include "DGKernel.h"
#include "InterfaceKernel.h"
#include "MooseVariableFEImpl.h"
#include "MooseVariableScalar.h"
#include "NonlocalKernel.h"
#include "NonlocalIntegratedBC.h"
#include "libmesh/threads.h"

ComputeFullJacobianThread::ComputeFullJacobianThread(FEProblemBase & fe_problem,
                                                     const std::set<TagID> & tags)
  : ComputeJacobianThread(fe_problem, tags),
    _nl(fe_problem.getNonlinearSystemBase()),
    _integrated_bcs(_nl.getIntegratedBCWarehouse()),
    _dg_kernels(_nl.getDGKernelWarehouse()),
    _interface_kernels(_nl.getInterfaceKernelWarehouse())
{
}

// Splitting Constructor
ComputeFullJacobianThread::ComputeFullJacobianThread(ComputeFullJacobianThread & x,
                                                     Threads::split split)
  : ComputeJacobianThread(x, split),
    _nl(x._nl),
    _integrated_bcs(x._integrated_bcs),
    _dg_kernels(x._dg_kernels),
    _interface_kernels(x._interface_kernels)
{
}

ComputeFullJacobianThread::~ComputeFullJacobianThread() {}

void
ComputeFullJacobianThread::computeJacobian()
{
  std::vector<std::pair<MooseVariableFEBase *, MooseVariableFEBase *>> & ce =
      _fe_problem.couplingEntries(_tid);
  for (const auto & it : ce)
  {
    MooseVariableFEBase & ivariable = *(it.first);
    MooseVariableFEBase & jvariable = *(it.second);

    unsigned int ivar = ivariable.number();
    unsigned int jvar = jvariable.number();

    if (ivariable.activeOnSubdomain(_subdomain) && jvariable.activeOnSubdomain(_subdomain) &&
        _warehouse->hasActiveVariableBlockObjects(ivar, _subdomain, _tid))
    {
      // only if there are dofs for j-variable (if it is subdomain restricted var, there may not be
      // any)
      const auto & kernels = _warehouse->getActiveVariableBlockObjects(ivar, _subdomain, _tid);
      for (const auto & kernel : kernels)
        if ((kernel->variable().number() == ivar) && kernel->isImplicit())
        {
          kernel->subProblem().prepareShapes(jvar, _tid);
          kernel->computeOffDiagJacobian(jvariable);
        }
    }
  }

  /// done only when nonlocal kernels exist in the system
  if (_fe_problem.checkNonlocalCouplingRequirement())
  {
    std::vector<std::pair<MooseVariableFEBase *, MooseVariableFEBase *>> & cne =
        _fe_problem.nonlocalCouplingEntries(_tid);
    for (const auto & it : cne)
    {
      MooseVariableFEBase & ivariable = *(it.first);
      MooseVariableFEBase & jvariable = *(it.second);

      unsigned int ivar = ivariable.number();
      unsigned int jvar = jvariable.number();

      if (ivariable.activeOnSubdomain(_subdomain) && jvariable.activeOnSubdomain(_subdomain) &&
          _warehouse->hasActiveVariableBlockObjects(ivar, _subdomain, _tid))
      {
        const auto & kernels = _warehouse->getActiveVariableBlockObjects(ivar, _subdomain, _tid);
        for (const auto & kernel : kernels)
        {
          std::shared_ptr<NonlocalKernel> nonlocal_kernel =
              std::dynamic_pointer_cast<NonlocalKernel>(kernel);
          if (nonlocal_kernel)
            if ((kernel->variable().number() == ivar) && kernel->isImplicit())
            {
              kernel->subProblem().prepareShapes(jvar, _tid);
              kernel->computeNonlocalOffDiagJacobian(jvar);
            }
        }
      }
    }
  }

  const std::vector<MooseVariableScalar *> & scalar_vars = _nl.getScalarVariables(_tid);
  if (scalar_vars.size() > 0)
  {
    // go over nl-variables (non-scalar)
    const std::vector<MooseVariableFEBase *> & vars = _nl.getVariables(_tid);
    for (const auto & ivariable : vars)
      if (ivariable->activeOnSubdomain(_subdomain) > 0 &&
          _warehouse->hasActiveVariableBlockObjects(ivariable->number(), _subdomain, _tid))
      {
        // for each variable get the list of active kernels
        const auto & kernels =
            _warehouse->getActiveVariableBlockObjects(ivariable->number(), _subdomain, _tid);
        for (const auto & kernel : kernels)
          if (kernel->isImplicit())
          {
            // now, get the list of coupled scalar vars and compute their off-diag jacobians
            const std::vector<MooseVariableScalar *> coupled_scalar_vars =
                kernel->getCoupledMooseScalarVars();

            // Do: dvar / dscalar_var, only want to process only nl-variables (not aux ones)
            for (const auto & jvariable : coupled_scalar_vars)
              if (_nl.hasScalarVariable(jvariable->name()))
                kernel->computeOffDiagJacobianScalar(jvariable->number());
          }
      }
  }
}

void
ComputeFullJacobianThread::computeFaceJacobian(BoundaryID bnd_id)
{
  std::vector<std::pair<MooseVariableFEBase *, MooseVariableFEBase *>> & ce =
      _fe_problem.couplingEntries(_tid);
  for (const auto & it : ce)
  {
    MooseVariableFEBase & ivar = *(it.first);
    MooseVariableFEBase & jvar = *(it.second);
    if (ivar.activeOnSubdomain(_subdomain) && jvar.activeOnSubdomain(_subdomain) &&
        _integrated_bcs.hasActiveBoundaryObjects(bnd_id, _tid))
    {
      // only if there are dofs for j-variable (if it is subdomain restricted var, there may not be
      // any)
      const std::vector<std::shared_ptr<IntegratedBCBase>> & bcs =
          _integrated_bcs.getBoundaryObjects(bnd_id, _tid);
      for (const auto & bc : bcs)
        if (bc->shouldApply() && bc->variable().number() == ivar.number() && bc->isImplicit())
        {
          bc->subProblem().prepareFaceShapes(jvar.number(), _tid);
          bc->computeJacobianBlock(jvar);
        }
    }
  }

  /// done only when nonlocal integrated_bcs exist in the system
  if (_fe_problem.checkNonlocalCouplingRequirement())
  {
    std::vector<std::pair<MooseVariableFEBase *, MooseVariableFEBase *>> & cne =
        _fe_problem.nonlocalCouplingEntries(_tid);
    for (const auto & it : cne)
    {
      MooseVariableFEBase & ivariable = *(it.first);
      MooseVariableFEBase & jvariable = *(it.second);

      unsigned int ivar = ivariable.number();
      unsigned int jvar = jvariable.number();

      if (ivariable.activeOnSubdomain(_subdomain) && jvariable.activeOnSubdomain(_subdomain) &&
          _integrated_bcs.hasActiveBoundaryObjects(bnd_id, _tid))
      {
        const std::vector<std::shared_ptr<IntegratedBCBase>> & integrated_bcs =
            _integrated_bcs.getBoundaryObjects(bnd_id, _tid);
        for (const auto & integrated_bc : integrated_bcs)
        {
          std::shared_ptr<NonlocalIntegratedBC> nonlocal_integrated_bc =
              std::dynamic_pointer_cast<NonlocalIntegratedBC>(integrated_bc);
          if (nonlocal_integrated_bc)
            if ((integrated_bc->variable().number() == ivar) && integrated_bc->isImplicit())
            {
              integrated_bc->subProblem().prepareFaceShapes(jvar, _tid);
              integrated_bc->computeNonlocalOffDiagJacobian(jvar);
            }
        }
      }
    }
  }

  const std::vector<MooseVariableScalar *> & scalar_vars = _nl.getScalarVariables(_tid);
  if (scalar_vars.size() > 0)
  {
    // go over nl-variables (non-scalar)
    const std::vector<MooseVariableFEBase *> & vars = _nl.getVariables(_tid);
    for (const auto & ivar : vars)
      if (ivar->activeOnSubdomain(_subdomain) > 0 &&
          _integrated_bcs.hasActiveBoundaryObjects(bnd_id, _tid))
      {
        // for each variable get the list of active kernels
        const std::vector<std::shared_ptr<IntegratedBCBase>> & bcs =
            _integrated_bcs.getActiveBoundaryObjects(bnd_id, _tid);
        for (const auto & bc : bcs)
          if (bc->variable().number() == ivar->number() && bc->isImplicit())
          {
            // now, get the list of coupled scalar vars and compute their off-diag jacobians
            const std::vector<MooseVariableScalar *> coupled_scalar_vars =
                bc->getCoupledMooseScalarVars();

            // Do: dvar / dscalar_var, only want to process only nl-variables (not aux ones)
            for (const auto & jvar : coupled_scalar_vars)
              if (_nl.hasScalarVariable(jvar->name()))
                bc->computeJacobianBlockScalar(jvar->number());
          }
      }
  }
}

void
ComputeFullJacobianThread::computeInternalFaceJacobian(const Elem * neighbor)
{
  if (_dg_kernels.hasActiveBlockObjects(_subdomain, _tid))
  {
    const auto & ce = _fe_problem.couplingEntries(_tid);
    for (const auto & it : ce)
    {
      const std::vector<std::shared_ptr<DGKernel>> & dgks =
          _dg_kernels.getActiveBlockObjects(_subdomain, _tid);
      for (const auto & dg : dgks)
      {
        MooseVariableFEBase & ivariable = *(it.first);
        MooseVariableFEBase & jvariable = *(it.second);

        unsigned int ivar = ivariable.number();
        unsigned int jvar = jvariable.number();

        if (dg->variable().number() == ivar && dg->isImplicit() &&
            dg->hasBlocks(neighbor->subdomain_id()) && jvariable.activeOnSubdomain(_subdomain))
        {
          dg->subProblem().prepareFaceShapes(jvar, _tid);
          dg->subProblem().prepareNeighborShapes(jvar, _tid);
          dg->computeOffDiagJacobian(jvar);
        }
      }
    }
  }
}

void
ComputeFullJacobianThread::computeInternalInterFaceJacobian(BoundaryID bnd_id)
{
  if (_interface_kernels.hasActiveBoundaryObjects(bnd_id, _tid))
  {
    const auto & ce = _fe_problem.couplingEntries(_tid);
    for (const auto & it : ce)
    {
      const std::vector<std::shared_ptr<InterfaceKernel>> & int_ks =
          _interface_kernels.getActiveBoundaryObjects(bnd_id, _tid);
      for (const auto & interface_kernel : int_ks)
      {
        if (!interface_kernel->isImplicit())
          continue;

        unsigned int ivar = it.first->number();
        unsigned int jvar = it.second->number();

        interface_kernel->subProblem().prepareFaceShapes(jvar, _tid);
        interface_kernel->subProblem().prepareNeighborShapes(jvar, _tid);

        if (interface_kernel->variable().number() == ivar)
          interface_kernel->computeElementOffDiagJacobian(jvar);

        if (interface_kernel->neighborVariable().number() == ivar)
          interface_kernel->computeNeighborOffDiagJacobian(jvar);
      }
    }
  }
}
