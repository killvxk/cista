#include <queue>
#include <set>

#include "doctest.h"

#include "sha1sum.h"

#include "cista.h"

using namespace cista;

namespace data = raw;

namespace {
struct node;

using node_id_t = uint32_t;

struct edge {
  data::ptr<node> from_;
  data::ptr<node> to_;
};

struct node {
  void add_edge(edge* e) { edges_.emplace_back(e); }
  node_id_t id() const { return id_; }

  node_id_t id_{0};
  node_id_t fill_{0};
  data::vector<data::ptr<edge>> edges_;
  data::string name_;
};

struct graph {
  node* make_node(data::string name) {
    return nodes_
        .emplace_back(data::make_unique<node>(
            node{next_node_id_++, 0, data::vector<data::ptr<edge>>{0u},
                 std::move(name)}))
        .get();
  }

  edge* make_edge(node_id_t const from, node_id_t const to) {
    return edges_
        .emplace_back(
            data::make_unique<edge>(edge{nodes_[from].get(), nodes_[to].get()}))
        .get();
  }

  data::vector<data::unique_ptr<node>> nodes_;
  data::vector<data::unique_ptr<edge>> edges_;
  node_id_t next_node_id_{0};
  node_id_t fill_{0};
};

inline std::set<node const*> bfs(node const* entry) {
  std::queue<node const*> q;
  std::set<node const*> visited;

  q.emplace(entry);

  while (!q.empty()) {
    auto const next = q.front();
    q.pop();

    if (visited.find(next) != end(visited)) {
      continue;
    }

    visited.emplace(next);

    for (auto const& e : next->edges_) {
      q.emplace(e->to_);
    }
  }

  return visited;
}

}  // namespace

TEST_CASE("graph raw serialize file") {
  {
    graph g;

    auto const n1 = g.make_node(data::string{"NODE A"});
    auto const n2 = g.make_node(data::string{"NODE B"});
    auto const n3 = g.make_node(data::string{"NODE C"});

    auto const e1 = g.make_edge(n1->id(), n2->id());
    auto const e2 = g.make_edge(n2->id(), n3->id());
    auto const e3 = g.make_edge(n3->id(), n1->id());

    n1->add_edge(e1);
    n2->add_edge(e2);
    n3->add_edge(e3);

    cista::sfile f{"test.bin", "wb"};
    cista::serialize(f, g);
  }  // EOL graph

  auto b = cista::file("test.bin", "r").content();
  CHECK("6eda6fb0f6d7b81c2c0ce0996e35cb4af608a64e" ==
        sha1sum::from_buf(std::vector<unsigned char>(b.begin(), b.end())));

  auto const g = data::deserialize<graph>(b);
  auto const visited = bfs(g->nodes_[0].get());
  unsigned i = 0;
  CHECK((*std::next(begin(visited), i++))->name_ == data::string{"NODE A"});
  CHECK((*std::next(begin(visited), i++))->name_ == data::string{"NODE B"});
  CHECK((*std::next(begin(visited), i++))->name_ == data::string{"NODE C"});
}

TEST_CASE("graph raw serialize buf") {
  cista::byte_buf buf;
  {
    graph g;

    auto const n1 = g.make_node(data::string{"NODE A"});
    auto const n2 = g.make_node(data::string{"NODE B"});
    auto const n3 = g.make_node(data::string{"NODE C"});

    auto const e1 = g.make_edge(n1->id(), n2->id());
    auto const e2 = g.make_edge(n2->id(), n3->id());
    auto const e3 = g.make_edge(n3->id(), n1->id());

    n1->add_edge(e1);
    n2->add_edge(e2);
    n3->add_edge(e3);

    buf = cista::serialize(g);
  }  // EOL graph

  CHECK("6eda6fb0f6d7b81c2c0ce0996e35cb4af608a64e" == sha1sum::from_buf(buf));

  auto const g = data::deserialize<graph>(buf);
  auto const visited = bfs(g->nodes_[0].get());
  unsigned i = 0;
  CHECK((*std::next(begin(visited), i++))->name_ == data::string{"NODE A"});
  CHECK((*std::next(begin(visited), i++))->name_ == data::string{"NODE B"});
  CHECK((*std::next(begin(visited), i++))->name_ == data::string{"NODE C"});
}