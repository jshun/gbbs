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

// Usage:
// numactl -i all ./PageRank -s -m -rounds 3 twitter_SJ
// flags:
//   optional:
//     -eps : the epsilon to use for convergence (1e-6 by default)
//     -rounds : the number of times to run the algorithm
//     -c : indicate that the graph is compressed
//     -m : indicate that the graph should be mmap'd
//     -s : indicate that the graph is symmetric

#include "PageRank.h"

template <class vertex>
double PageRank_runner(graph<vertex>& GA, commandLine P) {
  std::cout << "### Application: PageRank" << std::endl;
  std::cout << "### Graph: " << P.getArgument(0) << std::endl;
  std::cout << "### Threads: " << num_workers() << std::endl;
  std::cout << "### n: " << GA.n << std::endl;
  std::cout << "### m: " << GA.m << std::endl;
  std::cout << "### Params: -eps = " << P.getOptionDoubleValue("-eps", 0.000001) << std::endl;
  std::cout << "### ------------------------------------" << endl;

  timer t; t.start();
  double eps = P.getOptionDoubleValue("-eps", 0.000001);
  double local_eps = P.getOptionDoubleValue("-leps", 0.01);
  size_t iters = P.getOptionLongValue("-iters", 100);
  if (P.getOptionValue("-em")) {
    PageRank_edgeMap(GA, eps, iters);
  } else if (P.getOptionValue("-delta")) {
    delta::PageRankDelta(GA, eps, local_eps, iters);
  } else {
    PageRank(GA, eps, iters);
  }
  double tt = t.stop();

  std::cout << "### Running Time: " << tt << std::endl;
  return tt;
}

generate_main(PageRank_runner, false);
