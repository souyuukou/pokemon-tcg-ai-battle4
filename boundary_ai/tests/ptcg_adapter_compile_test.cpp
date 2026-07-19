#include "BoundarySearch.h"
#include "FixedList.h"

#include <stdexcept>

int main() {
    ptcg_boundary::FullyObservedDomain domain;
    ptcg_boundary::VisibleBoundaryValue value;
    boundary_ai::BoundaryDagSolver<
        ptcg_boundary::Position, ptcg_boundary::Action> solver(domain, value);
    (void)solver;

    FixedList<int, 2> list;
    list.push_back(1);
    list.push_back(2);
    if (!list.isFull()) throw std::runtime_error("FixedList full boundary");
    bool overflowRejected = false;
    try { list.push_front(0); } catch (...) { overflowRejected = true; }
    if (!overflowRejected) throw std::runtime_error("FixedList push_front overflow");
    bool rangeRejected = false;
    try { (void)list.take(2); } catch (...) { rangeRejected = true; }
    if (!rangeRejected) throw std::runtime_error("FixedList take range");
    return 0;
}
