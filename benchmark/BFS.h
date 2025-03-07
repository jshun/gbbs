// This code is part of the project "Theoretically Efficient Parallel Graph
// Algorithms Can Be Fast and Scalable", presented at Symposium on Parallelism
// in Algorithms and Architectures, 2018.
// Copyright (c) 2018 Laxman Dhulipala, Guy Blelloch, and Julian Shun
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all  copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include "ligra.h"

template <class W>
struct BFS_F {
  uintE* Parents;
  BFS_F(uintE* _Parents) : Parents(_Parents) {}
  inline bool update(const uintE& s, const uintE& d, const W& w) {
    if (Parents[d] == UINT_E_MAX) {
      Parents[d] = s;
      return 1;
    } else {
      return 0;
    }
  }
  inline bool updateAtomic(const uintE& s, const uintE& d, const W& w) {
    return (pbbslib::atomic_compare_and_swap(&Parents[d], UINT_E_MAX, s));
  }
  inline bool cond(const uintE& d) { return (Parents[d] == UINT_E_MAX); }
};

template <template <class W> class vertex, class W>
inline sequence<uintE> BFS(graph<vertex<W> >& GA, uintE src) {
  // Creates Parents array, initialized to all -1, except for src.
  auto Parents = sequence<uintE>(GA.n, [&](size_t i) { return UINT_E_MAX; });
  Parents[src] = src;

  vertexSubset Frontier(GA.n, src);
  size_t reachable = 0;
  while (!Frontier.isEmpty()) {
    std::cout << Frontier.size() << "\n";
    reachable += Frontier.size();
    vertexSubset output =
        edgeMap(GA, Frontier, BFS_F<W>(Parents.begin()), -1, sparse_blocked | dense_parallel);
    Frontier.del();
    Frontier = output;
  }
  Frontier.del();
  std::cout << "Reachable: " << reachable << "\n";
  return Parents;
}
