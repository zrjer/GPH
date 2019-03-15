#include <memory>
#include "core/AvaiLPSolver.h"
#include "core/Common.h"
#include "core/LPSolver.h"
#include "core/MIP.h"
#include "core/mpsreader.h"

int main() {
   MIP<double> mip;

   try {
      mip = mpsreader::parse("mip.mps");
   } catch (const std::exception& ex) {
      std::cout << ex.what();
      return 1;
   }

   try {
      std::unique_ptr<LPSolver<double>> solver(new AvaiLPSolver(mip));
      LPResult result = solver->solve();

      std::cout << "LP solver return status: " << to_str(result.status)
                << std::endl;

      if (result.status == LPResult::OPTIMAL) {
         std::cout << "obj: " << result.obj << std::endl;

         /*std::cout << "primal solution: ";
         for (auto val : result.primalSolution) std::cout << val << ", ";

         std::cout << "\ndual values: ";
         for (auto val : result.dualSolution) std::cout << val << ", ";

         bool feasible =
             checkFeasibility<double>(mip, result.primalSolution, 1e-9, 1e-6);

         std::cout << "\nfeasiblity check: " << feasible << std::endl;*/
      }

   } catch (...) {
      std::cout << "Solver raised an exception";
   }
   return 0;
}
