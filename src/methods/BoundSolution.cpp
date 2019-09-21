#include "BoundSolution.h"
#include "core/Common.h"
#include "core/Numerics.h"
#include "core/Propagation.h"
#include "io/Message.h"

#include <array>
#include <tbb/parallel_for.h>

// TODO fix binary first
// TODO this can be very slow on very dense problems

void
BoundSolution::search(const MIP& mip, const std::vector<double>& lb,
                      const std::vector<double>& ub,
                      const std::vector<Activity>& activities,
                      const LPResult&, const std::vector<double>&,
                      const std::vector<int>&,
                      std::shared_ptr<const LPSolver> solver,
                      SolutionPool& pool)
{
   int ncols = mip.getNCols();
   const auto& objective = mip.getObj();

   constexpr int nruns = 3;
   std::array<std::vector<double>, nruns> lower_bounds;
   std::array<std::vector<double>, nruns> upper_bounds;
   std::array<bool, nruns> feasible;

   auto run = [&](tbb::blocked_range<size_t>& range) {
      for (size_t i = range.begin(); i != range.end(); ++i)
      {
         upper_bounds[i] = ub;
         lower_bounds[i] = lb;

         switch (i)
         {
         case 0:
            feasible[i] = tryLBSolution(mip, lower_bounds[i],
                                        upper_bounds[i], activities);
            break;

         case 1:
            feasible[i] = tryUBSolution(mip, lower_bounds[i],
                                        upper_bounds[i], activities);
            break;

         case 2:
            feasible[i] = tryOptimisticSolution(
                mip, lower_bounds[i], upper_bounds[i], activities);
            break;

         default:
            assert(0);
         }
      }
   };

   tbb::parallel_for(tbb::blocked_range<size_t>(0, nruns), std::move(run));

   std::unique_ptr<LPSolver> localsolver;

   for (size_t i = 0; i < nruns; ++i)
   {
      if (!feasible[i])
         continue;

      if (mip.getStatistics().ncont == 0)
      {
         Message::debug("Bnd: found a solution");

         double obj = 0.0;
         for (int j = 0; j < ncols; ++j)
            obj += objective[i] * lower_bounds[i][j];

         pool.add(std::move(lower_bounds[i]), obj);
      }
      else
      {
         Message::debug("Bnd: solving local lp");

         if (!localsolver)
            localsolver = solver->clone();

         localsolver->changeBounds(lower_bounds[i], upper_bounds[i]);

         auto localresult = localsolver->solve(Algorithm::DUAL);
         if (localresult.status == LPResult::OPTIMAL)
         {
            Message::debug("Bnd: lb: lp feasible");

            assert(
                checkFeasibility<double>(mip, localresult.primalSolution));
            pool.add(std::move(localresult.primalSolution),
                     localresult.obj);
         }
         else if (localresult.status == LPResult::INFEASIBLE)
            Message::debug("Bnd: lb: lp infeasible");
      }
   }
}

bool
BoundSolution::tryUBSolution(const MIP& mip, std::vector<double>& locallb,
                             std::vector<double>& localub,
                             const std::vector<Activity>& activities) const
{
   int ncols = mip.getNCols();
   const auto& integer = mip.getInteger();

   auto local_activities = activities;

   std::vector<int> inflbIntVars;
   for (int col = 0; col < ncols; ++col)
   {
      if (integer[col] && locallb[col] != localub[col])
      {
         if (Num::isMinusInf(locallb[col]))
         {
            inflbIntVars.push_back(col);
            continue;
         }

         double oldub = localub[col];
         localub[col] = locallb[col];

         if (!propagate(mip, locallb, localub, local_activities, col,
                        locallb[col], oldub))
            return false;
      }
   }

   for (auto col : inflbIntVars)
   {
      double oldlb = locallb[col];
      double oldub = localub[col];

      // check if the bound have been changed by propagation
      if (!Num::isMinusInf(oldlb))
      {
         localub[col] = locallb[col];
      }
      else
      {
         if (Num::isInf(localub[col]))
         {
            locallb[col] = 0.0;
            localub[col] = 0.0;
         }
         else
            locallb[col] = localub[col];

         if (!propagate(mip, locallb, localub, local_activities, col,
                        oldlb, oldub))
            return false;
      }
   }

   return true;
}

bool
BoundSolution::tryLBSolution(const MIP& mip, std::vector<double>& locallb,
                             std::vector<double>& localub,
                             const std::vector<Activity>& activities) const
{
   int ncols = mip.getNCols();
   const auto& integer = mip.getInteger();

   auto local_activities = activities;

   std::vector<int> infubIntVars;
   for (int col = 0; col < ncols; ++col)
   {
      if (integer[col] && locallb[col] != localub[col])
      {
         if (Num::isInf(localub[col]))
         {
            infubIntVars.push_back(col);
            continue;
         }

         double oldlb = locallb[col];
         locallb[col] = localub[col];

         if (!propagate(mip, locallb, localub, local_activities, col,
                        oldlb, localub[col]))
            return false;
      }
   }

   for (auto col : infubIntVars)
   {
      double oldlb = locallb[col];
      double oldub = localub[col];
      // check if the bound have been changed by propagation
      if (!Num::isInf(oldub))
      {
         locallb[col] = localub[col];
      }
      else
      {
         if (Num::isMinusInf(locallb[col]))
         {
            locallb[col] = 0.0;
            localub[col] = 0.0;
         }
         else
            localub[col] = locallb[col];

         if (!propagate(mip, locallb, localub, local_activities, col,
                        oldlb, oldub))
            return false;
      }
   }

   return true;
}

bool
BoundSolution::tryOptimisticSolution(
    const MIP& mip, std::vector<double>& locallb,
    std::vector<double>& localub,
    const std::vector<Activity>& activities) const
{
   int ncols = mip.getNCols();
   const auto& objective = mip.getObj();
   const auto& integer = mip.getInteger();
   const auto& downLocks = mip.getDownLocks();
   const auto& upLocks = mip.getUpLocks();

   auto local_activities = activities;
   std::vector<int> varsToRound;

   for (int col = 0; col < ncols; ++col)
   {
      if (integer[col] && locallb[col] != localub[col])
      {
         double oldlb = locallb[col];
         double oldub = localub[col];

         if (objective[col] > 0.0)
         {
            if (Num::isMinusInf(locallb[col]))
            {
               varsToRound.push_back(col);
               continue;
            }

            localub[col] = locallb[col];
         }
         else if (objective[col] < 0.0)
         {
            if (Num::isInf(localub[col]))
            {
               varsToRound.push_back(col);
               continue;
            }

            locallb[col] = localub[col];
         }
         else
         {
            assert(objective[col] == 0.0);
            if (upLocks[col] > downLocks[col])
            {
               if (Num::isMinusInf(locallb[col]))
               {
                  varsToRound.push_back(col);
                  continue;
               }

               localub[col] = locallb[col];
            }
            else
            {
               if (Num::isInf(localub[col]))
               {
                  varsToRound.push_back(col);
                  continue;
               }

               locallb[col] = localub[col];
            }
         }

         assert(locallb[col] != Num::infval);
         assert(localub[col] != -Num::infval);

         if (!propagate(mip, locallb, localub, local_activities, col,
                        oldlb, oldub))
            return false;
      }
   }

   for (int col : varsToRound)
   {
      assert(integer[col]);

      bool lbinf = Num::isMinusInf(locallb[col]);
      bool ubinf = Num::isInf(localub[col]);

      double oldlb = locallb[col];
      double oldub = localub[col];

      if (!lbinf && !ubinf)
         continue;

      if (lbinf && ubinf)
      {
         locallb[col] = 0.0;
         localub[col] = 0.0;
      }
      else if (lbinf)
         locallb[col] = localub[col];
      else
         localub[col] = locallb[col];

      if (!propagate(mip, locallb, localub, local_activities, col, oldlb,
                     oldub))
         return false;
   }

   return true;
}
