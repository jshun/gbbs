// Copyright (c) 2019 Laxman Dhulipala, Guy Blelloch, and Julian Shun
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


#include "bridge.h"
#include "bucket.h"
#include "pbbslib/dyn_arr.h"
#include "pbbslib/sparse_table.h"
#include "edge_map_reduce.h"
#include "ligra.h"
#include "truss_utils.h"
#include <cassert>

// (1) One approach is to map each edge in a hash-table to its trussness. The
// keys need to be 8-byte aligned, and the keys are 8-byte values (tuples of
// ints), so even though we are only mapping m/2 edges, the total space usage is
// (m/2)*16 = m*8 bytes
//
// (2) The other approach is to store an array mapping each original edge to an
// identifier corresponding to its directed edge (m edge_t's). We also store an
// array from the directed edge identifiers to their trussness (m/2 ints). In
// the case where edge_t is a size_t, this requires m*8 + m/2*4 = m*10 bytes.

// The first approach seems marginally better. It is also a little simpler to
// query since we just have to look up an undirected edge in a HT in order to
// retrieve its trussness. We can also compare both approaches.

// For a large graph, like ClueWeb, with 74B edges, the edge-table will require
// ~600G of space.


template <template <typename W> class vertex, class W, class MT>
void initialize_trussness_values(graph<vertex<W>>& GA, MT& multi_table) {

  std::tuple<uintE, uintE> empty_tup = std::make_tuple<uintE, uintE>(UINT_E_MAX, 0);

  timer it; it.start();
  GA.map_edges([&] (const uintE& u, const uintE& v, const W& wgh) {
    if (u < v) {
      multi_table.insert(u, std::make_tuple(v,0));
    }
  });
  it.stop(); it.reportTotal("insertion time");

  // 2. Triangle count, update trussness scores for each edge
  // 2.(a) Rank vertices based on degree
  auto rank = truss_utils::rankNodes(GA.V, GA.n);

  // 2.(b)Direct edges to point from lower to higher rank vertices.
  auto pack_predicate =
      [&](const uintE& u, const uintE& v, const W& wgh) { return rank[u] < rank[v]; };
  auto DG = filter_graph<vertex, W>(GA, pack_predicate);

  // Each triangle only found once---inc all three edges
  // Question: how to send a value to a neighbor w/o significant contention?
  auto inc_truss_f = [&] (const uintE& u, const uintE& v, const uintE& w) {
    multi_table.increment(u, v);
    multi_table.increment(u, w);
    multi_table.increment(v, w);
  };
  timer tct; tct.start();
  truss_utils::TCDirected(DG, inc_truss_f);
  tct.stop(); tct.reportTotal("TC time");

  DG.del();
}

// High-level desc:
// 1. Compute a hash table mapping each edge (u, v), u < v, to its trussness The
// initial trussness values are just the number of triangles each edge
// participates in.
//
// 2. Next, we compute a bucketing where each edge is represented by its index
// in the HT (locs without an edge are in the "infinity" bucket, and never
// leave.
//
// 3. Peel. Each peeling step removes the edges in bucket k, which implicitly
// fixes their trussness numbers.
//   3.a Each edge intersects its two endpoints using the edges in the original,
//   undirected graph. For each neighbor w in intersect(N(u), N(v)), we find the
//   (u, w) and (v, w) edges in the hash-table, check if we should decrement
//   them, and if so, insert them (their ids) into a hashtable.
//
//   3.b Get the entries of the HT, actually decrement their coreness, see if
//   their bucket needs to be updated and if so, update.
template <template <typename W> class vertex, class W>
void KTruss_ht(graph<vertex<W> >& GA, size_t num_buckets = 16) {
  size_t n_edges = GA.m / 2;

  using edge_t = uintE;
  using bucket_t = uintE;
  using trussness_t = uintE;

  auto deg_lt = pbbs::delayed_seq<uintE>(GA.n, [&] (size_t i) { return GA.V[i].getOutDegree() < (1 << 15); });
  cout << "count = " << pbbslib::reduce_add(deg_lt) << endl;
  auto deg_lt_ct = pbbs::delayed_seq<size_t>(GA.n, [&] (size_t i) { if (GA.V[i].getOutDegree() < (1 << 15)) { return GA.V[i].getOutDegree(); } return (uintE)0;  });
  cout << "total degree = " << pbbslib::reduce_add(deg_lt_ct) << endl;

  auto counts = pbbs::sequence<size_t>(GA.n, (size_t)0);
  parallel_for(0, GA.n, [&] (size_t i) {
    auto d_i = GA.V[i].getOutDegree();
    bool d_i_lt = d_i <= (1 << 15);
    auto count_f = [&] (const uintE& u, const uintE& v, const W& wgh) {
      if (u < v) {
        return d_i_lt;
      }
    };
    counts[i] = GA.V[i].countOutNgh(i, count_f);
  });
  cout << "total lt ct = " << pbbslib::reduce_add(counts) << endl;

  std::tuple<edge_t, bucket_t> histogram_empty = std::make_tuple(std::numeric_limits<edge_t>::max(), 0);
  auto em = HistogramWrapper<edge_t, bucket_t>(GA.m/50, histogram_empty);

  // Store the initial trussness of each edge in the trussness table.
  auto multi_hash = [&] (uintE k) { return pbbslib::hash32(k); };
  auto get_size = [&] (size_t vtx) {
    auto count_f = [&] (uintE u, uintE v, W& wgh) {
      return vtx < v;
    };
    return GA.V[vtx].countOutNgh(vtx, count_f);
  };
  auto trussness_multi = truss_utils::make_multi_table<uintE, uintE>(GA.n, UINT_E_MAX, get_size);

  // Note that this multi-table business is a performance optimization. The
  // previous version is somewhere in git history; we should measure how much
  // using a multi-table helps.
  //
  // Laxman (6/12): experiment with making the multi_table oriented by degree.
  // This requires an extra random access when handling an edge to place it in
  // the proper orientation. The simple ordering is to use ids, but using
  // low-deg --> high-deg has the advantage of reducing the max hash-table size,
  // which could improve locality.
  // * for small enough vertices, use an array instead of a hash table.

  // Initially stores #triangles incident/edge.
  initialize_trussness_values(GA, trussness_multi);

  // Initialize the bucket structure. #ids = trussness table size
  auto get_bkt = pbbslib::make_sequence<uintE>(trussness_multi.size(), [&] (size_t i) {
    auto table_value = std::get<1>(trussness_multi.big_table[i]); // the trussness.
    return (uintE)table_value;
  });
  auto b = make_buckets<edge_t, bucket_t>(trussness_multi.size(), get_bkt, increasing, num_buckets);

  // Stores edges idents that lose a triangle, including duplicates (MultiSet)
  auto hash_edge_id = [&] (const edge_t& e) { return pbbs::hash32(e); };
  auto decr_source_table = make_sparse_table<edge_t, uintE>(1 << 20, std::make_tuple(std::numeric_limits<edge_t>::max(), (uintE)0), hash_edge_id);

  auto del_edges = pbbslib::dyn_arr<edge_t>(6*GA.n);
  auto actual_degree = pbbs::sequence<uintE>(GA.n, [&] (size_t i) {
    return GA.V[i].getOutDegree();
  });

//  size_t max_t = 0;
//  for (size_t i=0; i<trussness.size(); i++) {
//    if (std::get<0>(std::get<0>(trussness.table[i])) != UINT_E_MAX) {
//      auto tt = std::get<1>(trussness.table[i]);
//      if (tt > max_t) {
//        max_t = tt;
//      }
//    }
//  }
//  cout << "maxtt = " << max_t << endl;
//  exit(0);

  auto get_trussness_and_id = [&trussness_multi] (uintE u, uintE v) {
    // Precondition: uv is an edge in G.
    edge_t id = trussness_multi.idx(u, v);
    trussness_t truss = std::get<1>(trussness_multi.big_table[id]);
    return std::make_tuple(truss, id);
  };

  timer em_t, decrement_t, bt, peeling_t; peeling_t.start();
  size_t finished = 0, rho = 0, k_max = 0;
  size_t iter = 0;
  while (finished != n_edges) {
    bt.start();
    auto bkt = b.next_bucket();
    bt.stop();
    auto rem_edges = bkt.identifiers;
    if (rem_edges.size() == 0) { continue; }

    uintE k = bkt.id;
    finished += rem_edges.size();
    k_max = std::max(k_max, bkt.id);
//    cout << "k = " << k << " iter = " << iter << " #edges = " << rem_edges.size() << endl;

    if (k == 0 || finished == n_edges) {
      // No triangles incident to these edges. We set their trussness to MAX,
      // which is safe since there are no readers until we output.
      par_for(0, rem_edges.size(), [&] (size_t i) {
        edge_t id = rem_edges[i];
        std::get<1>(trussness_multi.big_table[id]) = std::numeric_limits<int>::max(); // UINT_E_MAX is reserved
      });
      continue;
    }

    size_t e_size = 2*k*rem_edges.size();
    size_t e_space_required  = (size_t)1 << pbbslib::log2_up((size_t)(e_size*1.2));

    // Resize the table that stores edge updates if necessary.
    decr_source_table.resize_no_copy(e_space_required);
    auto decr_tab = make_sparse_table<edge_t, uintE>(decr_source_table.table, e_space_required, std::make_tuple(std::numeric_limits<edge_t>::max(), (uintE)0), hash_edge_id, false /* do not clear */);

//    cout << "starting decrements" << endl;
    decrement_t.start();
    par_for(0, rem_edges.size(), 1, [&] (size_t i) {
      edge_t id = rem_edges[i];
      uintE u = trussness_multi.u_for_id(id);
      uintE v = std::get<0>(trussness_multi.big_table[id]);
      truss_utils::decrement_trussness<edge_t, trussness_t>(GA, id, u, v, decr_tab, get_trussness_and_id, k);
    });
    decrement_t.stop();
//    cout << "finished decrements" << endl;

    auto decr_edges = decr_tab.entries();
    parallel_for(0, decr_edges.size(), [&] (size_t i) {
      auto id_and_ct = decr_edges[i];
      edge_t id = std::get<0>(id_and_ct);
      uintE triangles_removed = std::get<1>(id_and_ct);
      uintE current_deg = std::get<1>(trussness_multi.big_table[id]);
      assert(current_deg > k);
      uintE new_deg = std::max(current_deg - triangles_removed, k);
      std::get<1>(trussness_multi.big_table[id]) = new_deg; // update
      bucket_t bkt = b.get_bucket(current_deg, new_deg);
      std::get<1>(decr_edges[i]) = bkt;
    });

    auto rebucket_edges = pbbs::filter(decr_edges, [&] (const std::tuple<edge_t, uintE>& eb) {
      return std::get<1>(eb) != UINT_E_MAX;
    });
    auto edges_moved_f = [&] (size_t i) {
      return Maybe<std::tuple<edge_t, bucket_t>>(rebucket_edges[i]); };

    bt.start();
    b.update_buckets(edges_moved_f, rebucket_edges.size());
    bt.stop();

//    auto apply_f = [&](const std::tuple<edge_t, uintE>& p)
//        -> const Maybe<std::tuple<edge_t, bucket_t> > {
//      edge_t id = std::get<0>(p);
//      uintE triangles_removed = std::get<1>(p);
//      uintE current_deg = std::get<1>(trussness_multi.big_table[id]);
//      if (current_deg > k) {
//        uintE new_deg = std::max(current_deg - triangles_removed, k);
//        std::get<1>(trussness_multi.big_table[id]) = new_deg;
//        bucket_t bkt = b.get_bucket(current_deg, new_deg);
//        return wrap(id, bkt);
//      }
//      return Maybe<std::tuple<edge_t, bucket_t>>();
//    };
//
//    em_t.start();
//    sequence<edge_t> edge_seq = decrement_tab.entries();
//    auto res = em.template edgeMapCount(edge_seq, apply_f);
//    em_t.stop();
//
//    auto rebucket_f = [&] (size_t i) -> Maybe<std::tuple<edge_t, bucket_t>> {
//      auto ret = Maybe<std::tuple<edge_t, bucket_t>>();
//      ret.t = res.second[i];
//      ret.exists = true;
//      return ret;
//    };
//    auto edges_moved_map = pbbslib::make_sequence<Maybe<std::tuple<edge_t, bucket_t>>>(res.first, rebucket_f);
//    auto edges_moved_f = [&] (size_t i) { return edges_moved_map[i]; };
//    bt.start();
//    b.update_buckets(edges_moved_f, edges_moved_map.size());
//    bt.stop();

    // Unmark edges removed in this round, and decrement their trussness.
    par_for(0, rem_edges.size(), [&] (size_t i) {
      edge_t id = rem_edges[i];
      std::get<1>(trussness_multi.big_table[id]) -= 1;
    });

    // Clear the table storing the edge decrements.
    decr_tab.clear();
    iter++;

    del_edges.copyIn(rem_edges, rem_edges.size());

    if (del_edges.size > 2*GA.n) {
      // compact
      cout << "compacting, " << del_edges.size << endl;
      // map over both endpoints, update counts using histogram
      // this is really a uintE seq, but edge_t >= uintE, and this way we can
      // re-use the same histogram structure.
      auto decr_seq = pbbs::sequence<edge_t>(2*del_edges.size);
      parallel_for(0, del_edges.size, [&] (size_t i) {
        size_t fst = 2*i; size_t snd = fst+1;
        edge_t id = del_edges.A[i];
        uintE u = trussness_multi.u_for_id(id);
        uintE v = std::get<0>(trussness_multi.big_table[id]);
        decr_seq[fst] = u;
        decr_seq[snd] = v;
      });

      // returns only those vertices that have enough degree lost to warrant
      // packing them out. Again note that edge_t >= uintE
      auto apply_vtx_f = [&](const std::tuple<edge_t, uintE>& p)
        -> const Maybe<std::tuple<edge_t, uintE> > {
        uintE id = std::get<0>(p);
        uintE degree_lost = std::get<1>(p);
        uintE prev_degree = actual_degree[id];
        actual_degree[id] -= degree_lost;
        uintE new_degree = prev_degree - degree_lost;
        // compare with GA.V[id]. this is the current space used for this vtx.
        return Maybe<std::tuple<edge_t, uintE>>();
      };

      em_t.start();
      em.template edgeMapCount(decr_seq, apply_vtx_f);
      em_t.stop();

      auto all_vertices = pbbs::delayed_seq<uintE>(GA.n, [&] (size_t i) { return i; });
      auto to_pack_seq = pbbs::filter(all_vertices, [&] (uintE u) {
        return 4*actual_degree[u] >= GA.V[u].getOutDegree();
      });
      auto to_pack = vertexSubset(GA.n, std::move(to_pack_seq));

      auto pack_predicate = [&](const uintE& u, const uintE& ngh, const W& wgh) {
        // return true iff edge is still alive
        trussness_t t_u_ngh;
        edge_t edgeid;
        std::tie(t_u_ngh, edgeid) = get_trussness_and_id(u, ngh);
        return t_u_ngh >= k;
      };
      edgeMapFilter(GA, to_pack, pack_predicate, pack_edges | no_output);

      del_edges.size = 0; // reset dyn_arr
    }
  }

  peeling_t.stop(); peeling_t.reportTotal("peeling time");
  bt.reportTotal("Bucketing time");
  em_t.reportTotal("EdgeMap time");
  decrement_t.reportTotal("Decrement trussness time");

  // == Important: The actual trussness is the stored trussness value + 1.
  // Edges with trussness 0 had their values stored as std::numeric_limits<int>::max()
  uint mx = 0;
  cout << "mx = " << mx << endl;
  cout << "iters = " << iter << endl;
}

