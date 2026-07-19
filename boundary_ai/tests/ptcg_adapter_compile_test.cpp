#include "BoundarySearch.h"

int main() {
    ptcg_boundary::FullyObservedDomain domain;
    ptcg_boundary::VisibleBoundaryValue value;
    boundary_ai::BoundaryDagSolver<
        ptcg_boundary::Position, ptcg_boundary::Action> solver(domain, value);
    (void)solver;
    return 0;
}
