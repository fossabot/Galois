#ifndef GRAPH_COLORING_DETERMINISTIC_H
#define GRAPH_COLORING_DETERMINISTIC_H

#include "GraphColoringBase.h"

#include "Galois/Runtime/KDGparaMeter.h"
#include "Galois/Runtime/ll/CompilerSpecific.h"
#include "Galois/gdeque.h"

#include <atomic>
#include <random>

////////////////////////////////////////////////////////////
struct EdgeDataDAG {
   unsigned color;
   EdgeDataDAG() :
         color(0) {
   }
};
struct NodeDataDAG {
   typedef EdgeDataDAG EdgeDataType;
   unsigned color;
   std::atomic<unsigned> indegree;
   unsigned priority;
   unsigned id;
//   typename Galois::gdeque<std::pair<unsigned, EdgeDataDAG*> > pred_data;

   NodeDataDAG(unsigned _id = 0) :
         indegree(0), priority(0), id(_id) {
   }

};
////////////////////////////////////////////////////////////
typedef Galois::Graph::LC_CSR_Graph<NodeDataDAG, EdgeDataDAG*>::with_numa_alloc<true>::type
// ::with_no_lockable<true>::type Graph;
::with_no_lockable<false>::type Graph;

typedef Graph::GraphNode GNode;

////////////////////////////////////////////////////////////
class GraphEdgeColoringDAG: public GraphColoringBase<Graph> {
   static const unsigned DEFAULT_CHUNK_SIZE = 8;

   typedef Galois::Runtime::PerThreadVector<unsigned> PerThrdColorVec;
   typedef typename Graph::GraphNode GN;
   typedef typename Graph::node_data_type NodeData;
   typedef typename Graph::edge_data_type EdgeData;
   typedef typename NodeData::EdgeDataType EdgeDataType;

   Graph graph;
   PerThrdColorVec perThrdColorVec;
   PerThrdColorVec perThrdNbrColorVec;
public:
   /////////////////////////////////////////////////////////////
   void readGraph(void) {
      /*     typedef Galois::Graph::FileGraph FileGraph;
       FileGraph filegraph;
       filegraph.fromFile(filename);
       graph.allocateFrom(filegraph);
       */
      Galois::Graph::readGraph(graph, filename);
      const size_t numNodes = graph.size();
      Galois::GAccumulator<size_t> numEdges;
      Galois::FixedSizeAllocator<EdgeDataType> heap;

      Galois::StatTimer t_init("initialization time: ");

      t_init.start();
      /*
       * Go over each node and allocate edges on one side (src<dst).
       * */
      Galois::on_each([&] (const unsigned tid, const unsigned numT) {

         size_t num_per = (numNodes + numT - 1) / numT;
         size_t beg = tid * num_per;
         size_t end = std::min (numNodes, (tid + 1) * num_per);

         auto it_beg = graph.begin ();
         std::advance (it_beg, beg);

         auto it_end = it_beg;
         std::advance (it_end, (end - beg));

         for (; it_beg != it_end; ++it_beg) {
            // graph.getData (*it_beg, Galois::NONE) = NodeData (beg++);
            GN src = *it_beg;
            auto* ndptr = &(graph.getData (src, Galois::NONE));
            ndptr->~NodeData();
            new (ndptr) NodeData (beg++);
            NodeData & nd = graph.getData(src,Galois::NONE);

            for(auto eit = graph.edge_begin(src, Galois::NONE); eit!=graph.edge_end(src,Galois::NONE); ++eit) {
               GN dst = graph.getEdgeDst(eit);

               if(src < dst ) {
                  EdgeDataType * edata= heap.allocate(1);
                  new (edata) EdgeDataType();
                  graph.getEdgeData(eit,Galois::NONE) = edata;
                  auto & dst = graph.getData(graph.getEdgeDst(eit), Galois::NONE);
                  graph.getEdgeData(eit, Galois::NONE)->color = 0;
                  NodeData &dd = graph.getData(graph.getEdgeDst(eit));
//                  dd.pred_data.push_back(std::pair<unsigned, EdgeData>(ndptr->id,edata) );
               }
            }

            size_t deg = std::distance (
                  graph.edge_begin (*it_beg, Galois::NONE),
                  graph.edge_end (*it_beg, Galois::NONE));

            numEdges.update (deg);
         }
      }, Galois::loopname("initialize"));
      /////////////////////////////////////////////////////////////////
      /*
       * Now update the edges for edge-directions missed in first stage.
       *
       * */
      Galois::do_all(graph.begin(), graph.end(), [&](GN src) {
         NodeData & sd = graph.getData(src, Galois::NONE);
         for(auto e = graph.edge_begin(src, Galois::NONE); e!=graph.edge_end(src, Galois::NONE); ++e) {
            GN dst = graph.getEdgeDst(e);
            NodeData & dd = graph.getData(dst);
            if(src > dst) {
               bool found = false;
               for(auto d_e = graph.edge_begin(dst, Galois::NONE); d_e != graph.edge_end(dst, Galois::NONE); ++d_e) {
                  if(graph.getEdgeDst(d_e)==src) {
                     found = true;
                     graph.getEdgeData(e,Galois::NONE) = graph.getEdgeData(d_e, Galois::NONE);
                     break;
                  }
               }
               if(!found)
               std::fprintf(stderr, "!![%zd],[%zd]!!\n", src,dst);
            } //end if

         }

      }, Galois::loopname("de-duplicate"));

      // color 0 is reserved as uncolored value
      // therefore, we put in at least 1 entry to handle the
      // corner case when a node with no neighbors is being colored
      for (unsigned i = 0; i < perThrdColorVec.numRows(); ++i) {
         perThrdColorVec.get(i).resize(1, 0);
      }

      t_init.stop();

      std::printf("Graph read with %zd nodes and %zd edges\n", numNodes, numEdges.reduceRO());

   }

   /*
    *The basic algorithm is as follows. Go over each node.
    * For each edge, intialize a nbr-colors with colors assigned so for
    * for this node. Now go over the destinations edges (dst). For each
    * edge of dst, update the nbr-colors to reflect colors that have been
    * assigned. Once finished, either pick the smallest unassigned color,
    * or assign a new color to the edge. Update the my-colors array to
    * reflect this.
    * */
   void colorEdges(GN src) {
      auto& sd = graph.getData(src, Galois::NONE);
      auto& forbiddenColors = perThrdColorVec.get(); //my colors
      auto& forbiddenNbrColors = perThrdNbrColorVec.get(); //per-edge destination colors

      std::fill(forbiddenColors.begin(), forbiddenColors.end(), unsigned(-1)); //reset my-colors.
      /*
       * Update forbidden colors with edges already colored, and also check already colored edges for
       * potential conflicts, attempting to resolve them locally.
       * */
      for (auto e = graph.edge_begin(src, Galois::NONE), e_end = graph.edge_end(src, Galois::NONE); e != e_end; ++e) {
         GN dst = graph.getEdgeDst(e);
         auto & dd = graph.getData(dst, Galois::NONE);
         unsigned color = graph.getEdgeData(e, Galois::NONE)->color;
         if (color != 0) { //Already colord.
            if (forbiddenColors.size() <= color) { //Resize if necessary
               forbiddenColors.resize(color + 1, (unsigned) -1);
            }
            if (forbiddenColors[color] != -1) { //if already taken, reset color
               graph.getEdgeData(e, Galois::NONE)->color = 0;
            } else
               //update forbidden colors.
               forbiddenColors[color] = sd.id;
         }
      }
      /*
       * Now actually go over the edges and color them.
       * */
      for (auto e = graph.edge_begin(src, Galois::NONE), e_end = graph.edge_end(src, Galois::NONE); e != e_end; ++e) {

         forbiddenNbrColors.resize(forbiddenColors.size(), -1);  //reset nbr colors
         std::copy(forbiddenColors.begin(), forbiddenColors.end(), forbiddenNbrColors.begin());  //copy from my-colors

         GN dst = graph.getEdgeDst(e);
         EdgeData & edata = graph.getEdgeData(e);
         if (edata->color == 0) { //uncolored
            //Go over edges of dst.
            for (auto e_d = graph.edge_begin(dst, Galois::NONE); e_d != graph.edge_end(dst, Galois::NONE); ++e_d) {
               EdgeData &d_eData = graph.getEdgeData(e_d);
               //If edge color is larger than currently assigned, update
               //both colors
               if (forbiddenColors.size() <= d_eData->color) {
                  forbiddenNbrColors.resize(d_eData->color + 1, unsigned(-1));
                  forbiddenColors.resize(d_eData->color + 1, unsigned(-1));
               } //end if forbiddenColors.size()

               //Update nbr-colors
               forbiddenNbrColors[d_eData->color] = sd.id;

            } //end for e_d iteration

            bool colored = false;
            //Go over all colors to find any unassigned colors
            for (size_t i = 1; i < forbiddenNbrColors.size(); ++i) {
               if (forbiddenNbrColors[i] == -1) {
                  edata->color = i;
                  colored = true;
                  break;
               } //End if color found
            } //End for colors

            if (!colored) {
               edata->color = forbiddenNbrColors.size();
               forbiddenColors.resize(forbiddenNbrColors.size() + 1);

            }
            forbiddenColors[edata->color] = sd.id;
         }
      }
      // std::printf ("Node %d assigned color %d\n", sd.id, sd.color);
   }

   /////////////////////////////////////////////////////////////
   /*
    * Verify edge-coloring.
    * */
   void verify(void) {
      if (skipVerify) {
         return;
      }

      Galois::StatTimer t_verify("verification time: ");

      t_verify.start();

      Galois::GReduceLogicalOR foundError;
      Galois::GReduceMax<unsigned> maxColor;

      Galois::do_all_choice(Galois::Runtime::makeLocalRange(graph), [&] (GN src) {
         auto& sd = graph.getData (src, Galois::NONE);
         for (auto e = graph.edge_begin (src, Galois::NONE),
               e_end = graph.edge_end (src, Galois::NONE); e != e_end; ++e) {

            GN dst = graph.getEdgeDst (e);
            auto & dd = graph.getData(dst, Galois::NONE);
            EdgeData & eData = graph.getEdgeData(e);
            if(eData->color==0) {
               std::fprintf(stderr, "ERROR! : Edge %d not assigned color \n", sd.id);
               foundError.update(true);
            }
            //auto& dd = graph.getData (dst, Galois::NONE);
            for(auto e_in = graph.edge_begin(dst,Galois::NONE); e_in!=graph.edge_end(dst, Galois::NONE); ++e_in) {
               EdgeData & in_eData = graph.getEdgeData(e_in);
               GN o_dst = graph.getEdgeDst(e_in);
               if(o_dst!=src && eData->color == in_eData->color) {
                  foundError.update(true);
                  std::fprintf(stderr, "ERROR!: Edge %d and %d have same color : %d \n", sd.id,dd.id, eData->color);
                  printNode(src);
                  printNode(dst);
                  exit(-1);
               }
            }
            maxColor.update (eData->color);

         }

      }, "check-edge-coloring", Galois::doall_chunk_size<DEFAULT_CHUNK_SIZE>());

      std::printf("Graph-edges colored with %d colors\n", maxColor.reduce());

      t_verify.stop();

      if (foundError.reduceRO()) {
         GALOIS_DIE("ERROR! verification failed!\n");
      } else {
         printf("OK! verification succeeded!\n");
      }
   }

   void printNode(GN n) {
      std::fprintf(stderr, "Node[%zd]:{", graph.getData(n, Galois::NONE).id);
      for (auto eit = graph.edge_begin(n); eit != graph.edge_end(n); ++eit) {
         GN dst = graph.getEdgeDst(eit);
         std::fprintf(stderr, "%zd(%d),", graph.getEdgeData(eit)->color, graph.getData(dst).id);
      }
//      std::fprintf(stderr, "}PredColors {\n");
//      for (auto e : graph.getData(n, Galois::NONE).pred_data) {
//         std::fprintf(stderr, "%zd, ", e.second->color);
//      }
      std::fprintf(stderr, "}\n");
   }

protected:

   ////////////////////////////////////////////////////////////
   struct NodeDataComparator {
      static bool compare(const NodeDataDAG& left, const NodeDataDAG& right) {
         if (left.priority != right.priority) {
            return left.priority < right.priority;
         } else {
            return left.id < right.id;
         }
      }

      bool operator ()(const NodeDataDAG& left, const NodeDataDAG& right) const {
         return compare(left, right);
      }
   };

   ////////////////////////////////////////////////////////////
   template<typename W>
   void initDAG(W& initWork) {
      NodeDataComparator cmp;

      Galois::do_all_choice(Galois::Runtime::makeLocalRange(graph), [&] (GNode src) {
         auto& sd = graph.getData (src, Galois::NONE);

         // std::printf ("Processing node %d with priority %d\n", sd.id, sd.priority);

            unsigned addAmt = 0;
            for (Graph::edge_iterator e = graph.edge_begin (src, Galois::NONE),
                  e_end = graph.edge_end (src, Galois::NONE); e != e_end; ++e) {
               GNode dst = graph.getEdgeDst (e);
               auto& dd = graph.getData (dst, Galois::NONE);

               if (cmp (dd, sd)) { // dd < sd
                  ++addAmt;
               }
            }

            // only modify the node being processed
            // if we modify neighbors, each node will be
            // processed twice.
            sd.indegree += addAmt;

            if (addAmt == 0) {
               assert (sd.indegree == 0);
               initWork.push (src);
            }
         }, "init-dag", Galois::doall_chunk_size<DEFAULT_CHUNK_SIZE>());

   }
   ////////////////////////////////////////////////////////////
   struct ColorNodeEdgeDAG {
      typedef int tt_does_not_need_aborts;
      GraphEdgeColoringDAG& outer;

      template<typename C>
      void operator ()(GNode src, C& ctx) {

         Graph& graph = outer.graph;

         auto& sd = graph.getData(src, Galois::NONE);
         assert(sd.indegree == 0);

         outer.colorEdges(src);

         for (Graph::edge_iterator e = graph.edge_begin(src, Galois::NONE), e_end = graph.edge_end(src, Galois::NONE); e != e_end; ++e) {

            GNode dst = graph.getEdgeDst(e);
            auto& dd = graph.getData(dst, Galois::NONE);
            // std::printf ("Neighbor %d has indegree %d\n", dd.id, unsigned(dd.indegree));
            unsigned x = --(dd.indegree);
            if (x == 0) {
               ctx.push(dst);
            }
         }
      }
   };

   ////////////////////////////////////////////////////////////
   void colorDAG(void) {

      Galois::InsertBag<GNode> initWork;

      Galois::StatTimer t_dag_init("dag initialization time: ");

      t_dag_init.start();
      initDAG(initWork);
      t_dag_init.stop();

      typedef Galois::WorkList::AltChunkedFIFO<DEFAULT_CHUNK_SIZE> WL_ty;

      std::printf("Number of initial sources: %zd\n", std::distance(initWork.begin(), initWork.end()));

      Galois::StatTimer t_dag_color("dag edge-coloring time: ");

      t_dag_color.start();
      Galois::for_each_local(initWork, ColorNodeEdgeDAG { *this }, Galois::loopname("edge-color-DAG"), Galois::wl<WL_ty>());
      t_dag_color.stop();
   }
   ////////////////////////////////////////////////////////////
   struct VisitNhood {

      static const unsigned CHUNK_SIZE = DEFAULT_CHUNK_SIZE;

      GraphEdgeColoringDAG& outer;

      template<typename C>
      void operator ()(GNode src, C&) {
         Graph& graph = outer.graph;
         NodeData& sd = graph.getData(src, Galois::CHECK_CONFLICT);
         for (Graph::edge_iterator e = graph.edge_begin(src, Galois::CHECK_CONFLICT), e_end = graph.edge_end(src, Galois::CHECK_CONFLICT); e != e_end; ++e) {
            GNode dst = graph.getEdgeDst(e);
         }
      }
   };

   ////////////////////////////////////////////////////////////
   struct ApplyOperator {
      static const unsigned CHUNK_SIZE = DEFAULT_CHUNK_SIZE;
      GraphEdgeColoringDAG& outer;

      template<typename C>
      void operator ()(GNode src, C&) {
         outer.colorEdges(src);
      }
   };

   ////////////////////////////////////////////////////////////
   void colorKDGparam(void) {

      struct NodeComparator {
         Graph& graph;

         bool operator ()(GNode ln, GNode rn) const {
            const auto& ldata = graph.getData(ln, Galois::NONE);
            const auto& rdata = graph.getData(rn, Galois::NONE);
            return NodeDataComparator::compare(ldata, rdata);
         }
      };

      ////////////////////////////////////////////////////////////
      Galois::Runtime::for_each_ordered_2p_param(Galois::Runtime::makeLocalRange(graph), NodeComparator { graph }, VisitNhood { *this }, ApplyOperator { *this },
            "coloring-ordered-param");
   }

   ////////////////////////////////////////////////////////////
   virtual void colorGraph(void) {
      assignPriority();

      if (useParaMeter) {
         colorKDGparam();
      } else {
         colorDAG();
      }
   }

   ////////////////////////////////////////////////////////////
public:
   virtual void color_edges(int argc, char* argv[]) {
         LonestarStart(argc, argv, name, desc, url);
         Galois::StatManager sm;

         readGraph();

         Galois::preAlloc(Galois::getActiveThreads() + 2 * sizeof(NodeData) * graph.size() / Galois::Runtime::MM::hugePageSize);
         Galois::reportPageAlloc("MeminfoPre");

         Galois::StatTimer t;

         t.start();
         colorGraph();
         t.stop();

         Galois::reportPageAlloc("MeminfoPost");

         verify();
      }

};

#endif // GRAPH_COLORING_DETERMINISTIC_H