# `std:graph`

Things, and what connects them.

```ts
import { Graph } from "std:graph";

const deps = new Graph(true);
deps.addEdge("app", "lib").addEdge("lib", "core");
deps.topologicalOrder().value;      // ["app", "lib", "core"]
```

A dependency order, a shortest route, which items belong to the same cluster:
one structure and four walks of it. Written out because the alternative is the
same adjacency Map rebuilt in every program, with the cycle check left out of
the version that needed it most.

## Specification

### `Graph`

`new Graph(directed = false)` — decided once, because an undirected graph is
the same structure with every edge added both ways and getting that wrong
halfway through is a bug that looks like bad data.

| Member | Signature | Behaviour |
| --- | --- | --- |
| `addNode` / `addEdge` | `(node)` / `(from, to, weight = 1)` | adding an edge adds its endpoints |
| `removeNode` / `removeEdge` | `(node)` / `(from, to)` | removing a node removes every edge that mentioned it |
| `hasNode` / `hasEdge` / `weight` | `(node)` / `(from, to)` | whether it is there, and what it weighs |
| `nodes` / `edges` / `neighbours` / `degree` | `()` / `(node)` | an undirected edge is listed **once** |
| `size` / `edgeCount` / `isDirected` | getters | how many nodes, how many edges, and which kind |

### Walking

| Member | Signature | Answers |
| --- | --- | --- |
| `breadthFirst` | `(start)` | a generator, nearest first |
| `depthFirst` | `(start)` | a generator, on an explicit stack |
| `reachableFrom` | `(start)` | those nodes as an array |
| `components` | `()` | each group that can reach one another, ignoring direction |

### Questions

| Member | Signature | Answers |
| --- | --- | --- |
| `shortestPath` | `(from, to)` | `{ path, distance }` by weight, or `undefined` |
| `topologicalOrder` | `()` | `Result` of the order, or of the cycle that prevents one |
| `hasCycle` | `()` | a boolean, directed or not |

### `DisjointSet`

Union-find: which things are in the same group, in almost constant time.

| Member | Signature | Behaviour |
| --- | --- | --- |
| `add` / `find` | `(item)` | the group's representative, flattening the path |
| `union` | `(a, b)` | joins them; **answers whether they were apart** |
| `connected` | `(a, b)` | whether they share a group |
| `groups` / `groupSize` / `groupCount` / `size` | `()` / `(item)` | the groups, one group's size, how many there are, and how many items |

## Implementation

**A node is anything usable as a Map key** — a string, a number, an object —
because the identifiers a program already has should not need renaming to be
put in a graph.

**Depth-first uses an explicit stack**, not recursion: a graph deep enough to
matter is deep enough to overflow. Its neighbours are pushed in reverse so the
first one is visited first, which a stack would otherwise get backwards.

**`shortestPath` refuses a negative weight** rather than answering wrongly.
Dijkstra assumes a settled path cannot improve, which a negative edge breaks —
so the failure is loud instead of a route that is merely not the shortest.

**`topologicalOrder` names the cycle.** Kahn's algorithm repeatedly takes a node
nothing points at, and what is left over when none remains *is* the cycle — so
the error can list the nodes involved rather than reporting that one exists.

**`union` answers whether the two were apart**, which is exactly the test
Kruskal's algorithm needs: an edge joining two groups belongs to the tree, and
one joining a group to itself would close a cycle.

**Both union-find optimisations are here** — the path is flattened on every
lookup and the smaller tree hangs under the larger. Without them it degrades to
a linked list and the whole point is lost.

**`components` ignores direction.** The strongly-connected version is a
different algorithm, and calling this one by that name would be a lie.

## Source

[`graph.cx`](graph.cx). `#undirectedWalk` is private.
