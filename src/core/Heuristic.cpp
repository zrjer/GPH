#include "Heuristic.h"
#include "MySolver.h"
#include "Numerics.h"
#include "Timer.h"
#include "io/Message.h"
#include "io/SOLFormat.h"

#include "ska/Hash.hpp"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <cassert>
#include <mutex>

Search::Search(std::initializer_list<FeasibilityHeuristic*> feas_heur_list,
               std::initializer_list<ImprovementHeuristic*> impr_heur_list,
               const Config& config)
{
   HashMap<std::string, int> feas_heur_name_to_id;
   for (auto heur : feas_heur_list)
   {
      feas_heur_name_to_id[heur->getName()] = feas_heuristics.size();
      feas_heuristics.emplace_back(heur);
   }

   HashMap<std::string, int> impr_heur_name_to_id;
   for (auto heur : impr_heur_list)
   {
      impr_heur_name_to_id[heur->getName()] = impr_heuristics.size();
      impr_heuristics.emplace_back(heur);
   }

   feas_solutions_pools.resize(feas_heuristics.size() + 1);
   impr_solutions_pools.resize(impr_heuristics.size());

   // pass configuration to heuristics
   try
   {
      for (auto [heur_name, param_name, value] : config)
      {
         auto iter = feas_heur_name_to_id.find(heur_name);
         if (iter != feas_heur_name_to_id.end())
         {
            int id = (*iter).second;
            feas_heuristics[id]->setParam(param_name, value);
         }
         else
         {
            iter = impr_heur_name_to_id.find(heur_name);
            if (iter != impr_heur_name_to_id.end())
            {
               int id = (*iter).second;
               impr_heuristics[id]->setParam(param_name, value);
            }
            else
               Message::warn("Parameter {} heuristic {} was ignored.",
                             param_name, heur_name);
         }
      }
   }
   catch (const std::bad_variant_access&)
   {
      Message::error("Value type error in the configuration file");
      throw;
   }
}

std::tuple<int, int, double, int>
Search::getFeasSolSummary() const
{
   int feas_min_cost_heur = -1;
   int feas_min_cost_sol = -1;
   double min_cost = std::numeric_limits<double>::max();
   int nsols = 0;

   for (size_t i = 0; i < feas_solutions_pools.size(); ++i)
   {
      nsols += feas_solutions_pools[i].size();

      for (size_t j = 0; j < feas_solutions_pools[i].size(); ++j)
      {
         if (feas_solutions_pools[i][j].second < min_cost)
         {
            min_cost = feas_solutions_pools[i][j].second;
            feas_min_cost_heur = i;
            feas_min_cost_sol = j;
         }
      }
   }

   return {feas_min_cost_heur, feas_min_cost_sol, min_cost, nsols};
}

std::tuple<int, int, double, int>
Search::getImprSolSummary() const
{
   int impr_min_cost_heur = -1;
   int impr_min_cost_sol = -1;
   double min_cost = std::numeric_limits<double>::max();
   int nsols = 0;

   for (size_t i = 0; i < impr_solutions_pools.size(); ++i)
   {
      nsols += impr_solutions_pools[i].size();

      for (size_t j = 0; j < impr_solutions_pools[i].size(); ++j)
      {
         if (impr_solutions_pools[i][j].second < min_cost)
         {
            min_cost = impr_solutions_pools[i][j].second;
            impr_min_cost_heur = i;
            impr_min_cost_sol = j;
         }
      }
   }

   return {impr_min_cost_heur, impr_min_cost_sol, min_cost, nsols};
}

bool
Search::checkSolFeas(const MIP& mip) const
{
   for (size_t i = 0; i < feas_solutions_pools.size(); ++i)
   {
      for (size_t j = 0; j < feas_solutions_pools[i].size(); ++j)
      {
         if (!checkFeasibility(mip, feas_solutions_pools[i][j].first, 1e-9,
                               1e-6))
         {
            Message::debug("{} solution number {} is INFEASIBLE",
                           feas_heuristics[i]->getName(), j);
            return false;
         }
      }
   }
   return true;
}

std::optional<std::vector<double>>
Search::run(const MIP& mip, int seconds)
{
   TimeLimit tlimit(Timer::now(), seconds);
   auto st = mip.getStats();
   auto lpSolver = std::make_shared<MySolver>(mip);

#ifdef NDEBUG
   Message::print("Problem has {} columns, {} rows, {} non-zeros",
                  st.ncols, st.nrows, st.nnzmat);
#endif

   // variables to be captured by the lambda
   LPResult result;

   Message::print("Solving root LP:");
   auto t0 = Timer::now();
   result = lpSolver->solve(Algorithm::DUAL);
   auto t1 = Timer::now();

   if (result.status != LPResult::OPTIMAL)
   {
      Message::print("The LP solver returned with status {}",
                     to_str(result.status));
      return {};
   }

#ifndef NDEBUG
   auto lpFeas = checkFeasibility<double, true>;
   assert(lpFeas(mip, result.primalSolution, 1e-9, 1e-6));
#endif

   roundFeasIntegers(result.primalSolution, st.nbin + st.nint);

   // TODO
   auto lpSolAct = computeSolActivities(mip, result.primalSolution);
   auto fractional =
       getFractional(result.primalSolution, st.nbin + st.nint);
   auto activities = computeActivities(mip);

   double percfrac = 100.0 * static_cast<double>(fractional.size()) /
                     (st.nbin + st.nint);

   Message::print("  {:<15}: {:0.2f} sec.", "Solving Time",
                  Timer::seconds(t1, t0));
   Message::print("  {:<15}: {:0.2f}", "Objective", result.obj);
   Message::print("  {:<15}: {} ({:0.1f}%)", "Frationals",
                  fractional.size(), percfrac);
   Message::print("");

   // if the LP can be trivially rounded to an integer solutions
   if (auto optSol = minLockRound(mip, result.primalSolution, result.obj,
                                  fractional))
   {
      auto& sol = optSol.value();
      Message::debug("Root lp can be rounded, obj {}", sol.second);

      feas_solutions_pools.back().add(std::move(sol.first), sol.second);
   }

   auto run_feas = [&](tbb::blocked_range<size_t>& range) -> void {
      for (size_t i = range.begin(); i != range.end(); ++i)
         feas_heuristics[i]->execute(
             mip, mip.getLB(), mip.getUB(), activities, result, lpSolAct,
             fractional, lpSolver, tlimit, feas_solutions_pools[i]);
   };

   Message::print("Running feasibility heuristics:");
   tbb::parallel_for(tbb::blocked_range<size_t>{0, feas_heuristics.size()},
                     std::move(run_feas));
   auto tend = Timer::now();

   assert(checkSolFeas(mip));

   auto [feas_min_cost_heur, feas_min_cost_sol, feas_min_cost,
         feas_nsols] = getFeasSolSummary();

   if (feas_nsols == 0)
   {
      Message::print("No solution found after {} sec.",
                     Timer::seconds(tend, t0));
      return {};
   }

   assert(feas_min_cost_sol != -1 && feas_min_cost_heur != -1);

   auto best_sol =
       feas_solutions_pools[feas_min_cost_heur][feas_min_cost_sol].first;
   double best_cost =
       feas_solutions_pools[feas_min_cost_heur][feas_min_cost_sol].second;

   checkFeasibility(mip, best_sol, 1e-9, 1e-6);

   // TODO checkfeasiblity after this
   // maxOutSolution(mip, best_sol, best_cost);

   checkFeasibility(mip, best_sol, 1e-6, 1e-9);

   double gap = 100.0 * std::fabs(feas_min_cost - result.obj) /
                (std::fabs(result.obj) + 1e-6);

   if (gap < 10000.0)
      Message::print(
          "Found {} solutions with gap {:0.2f}% after {:0.2} sec.",
          feas_nsols, gap, Timer::seconds(tend, t0));
   else
      Message::print("Found {} solutions with gap --- after {:0.2} sec.",
                     feas_nsols, gap, Timer::seconds(tend, t0));

   Message::print("  {:<15} {:<15} {:<10} {:<15}", "heuristic",
                  "Runtime (sec.)", "found", "objective");
   for (size_t i = 0; i < feas_heuristics.size(); ++i)
   {
      // TODO
      fmt::memory_buffer buf;
      if (feas_solutions_pools[i].size() == 0)
         fmt::format_to(buf, "{}", "--");
      else
         fmt::format_to(buf, "{:0.2f}", feas_solutions_pools[i][0].second);

      if (i == static_cast<size_t>(feas_min_cost_heur))
         Message::print("  {:<15} {:<15.1f} {:<10} {:<}*",
                        feas_heuristics[i]->getName(),
                        feas_heuristics[i]->getRunTime(),
                        feas_solutions_pools[i].size(), to_string(buf));
      else
         Message::print("  {:<15} {:<15.1f} {:<10} {:<}",
                        feas_heuristics[i]->getName(),
                        feas_heuristics[i]->getRunTime(),
                        feas_solutions_pools[i].size(), to_string(buf));
   }
   Message::print("");

   // return, don't run improvement heuristics
   if (feas_nsols == 0)
      return {};

   // run improvement heuristics
   auto run_impr = [&](tbb::blocked_range<size_t>& range) -> void {
      for (size_t i = range.begin(); i != range.end(); ++i)
         impr_heuristics[i]->execute(
             mip, mip.getLB(), mip.getUB(), activities, result, lpSolAct,
             fractional, best_sol, best_cost, lpSolver, tlimit,
             impr_solutions_pools[i]);
   };

   Message::print("Running improvement heuristics:");
   tbb::parallel_for(tbb::blocked_range<size_t>{0, impr_heuristics.size()},
                     std::move(run_impr));
   tend = Timer::now();

   auto [impr_min_cost_heur, impr_min_cost_sol, impr_min_cost,
         impr_nsols] = getImprSolSummary();

   if (impr_nsols > 0)
   {
      assert(impr_min_cost_sol != -1 && impr_min_cost_heur != -1);

      double gap = 100.0 * std::fabs(impr_min_cost - result.obj) /
                   (std::fabs(result.obj) + 1e-6);

      if (gap < 10000.0)
         Message::print(
             "Found {} improved solutions with gap {:0.2f}% after "
             "{:0.2} sec.",
             impr_nsols, gap, Timer::seconds(tend, t0));
      else
         Message::print(
             "Found {} solutions with gap --- after {:0.2} sec.",
             impr_nsols, Timer::seconds(tend, t0));

      Message::print("  {:<15} {:<15} {:<10} {:<15}", "heuristic",
                     "Runtime (sec.)", "found", "objective");
      for (size_t i = 0; i < impr_heuristics.size(); ++i)
      {
         fmt::memory_buffer buf;
         if (impr_solutions_pools[i].size() == 0)
            fmt::format_to(buf, "{}", "--");
         else
            fmt::format_to(buf, "{:0.2f}",
                           impr_solutions_pools[i][0].second);

         if (i == static_cast<size_t>(impr_min_cost_heur))
            Message::print("  {:<15} {:<15.1f} {:<10} {:<}*",
                           impr_heuristics[i]->getName(),
                           impr_heuristics[i]->getRunTime(),
                           impr_solutions_pools[i].size(), to_string(buf));
         else
            Message::print("  {:<15} {:<15.1f} {:<10} {:<}",
                           impr_heuristics[i]->getName(),
                           impr_heuristics[i]->getRunTime(),
                           impr_solutions_pools[i].size(), to_string(buf));
      }

      return impr_solutions_pools[impr_min_cost_heur][impr_min_cost_sol]
          .first;
   }
   else
      Message::print("No improved solution found");

   return feas_solutions_pools[feas_min_cost_heur][feas_min_cost_sol]
       .first;
}
