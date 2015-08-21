
#include "mjolnir/graphvalidator.h"
#include "mjolnir/graphtilebuilder.h"
#include "mjolnir/statistics.h"

#include <valhalla/midgard/logging.h>

#include <ostream>
#include <boost/format.hpp>
#include <sstream>
#include <iostream>
#include <string>
#include <vector>
#include <utility>
#include <queue>
#include <list>
#include <thread>
#include <future>
#include <mutex>
#include <numeric>
#include <set>
#include <unordered_map>

#include <valhalla/midgard/logging.h>
#include <valhalla/midgard/pointll.h>
#include <valhalla/midgard/distanceapproximator.h>
#include <valhalla/baldr/tilehierarchy.h>
#include <valhalla/baldr/graphid.h>
#include <valhalla/baldr/graphconstants.h>
#include <valhalla/baldr/graphreader.h>

using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::mjolnir;

namespace {

// Get the GraphId of the opposing edge.
uint32_t GetOpposingEdgeIndex(const GraphId& startnode, DirectedEdge& edge,
                              GraphReader& graphreader_, uint32_t& dupcount_, std::string& endnodeiso_, std::mutex& lock) {

  // Get the tile at the end node and get the node info
  GraphId endnode = edge.endnode();
  lock.lock();
  const GraphTile* tile = graphreader_.GetGraphTile(endnode);
  lock.unlock();
  const NodeInfo* nodeinfo = tile->node(endnode.id());

  // Set the end node iso.  Used for country crossings.
  endnodeiso_ = tile->admin(nodeinfo->admin_index())->country_iso();

  // TODO - check if more than 1 edge has matching startnode and
  // distance!

  // Get the directed edges and return when the end node matches
  // the specified node and length matches
  constexpr uint32_t absurd_index = 777777;
  uint32_t opp_index = absurd_index;
  const DirectedEdge* directededge = tile->directededge(
      nodeinfo->edge_index());
  for (uint32_t i = 0; i < nodeinfo->edge_count(); i++, directededge++) {
    // End node must match the start node, shortcut (bool) must match
    // and lengths must match
    if (directededge->endnode() == startnode &&
        edge.is_shortcut() == directededge->is_shortcut() &&
        directededge->length() == edge.length()) {
      if (opp_index != absurd_index) {
        dupcount_++;
      }
      opp_index = i;
    }
  }

  if (opp_index == absurd_index) {
    if (edge.use() >= Use::kRail) {
      // Ignore any except for parent / child stop connections and
      // stop-road connections
      // TODO - verify if we need opposing directed edges for transit lines
      if (edge.use() == Use::kTransitConnection)
        LOG_ERROR("No opposing transit connection edge: endstop = " +
                  std::to_string(nodeinfo->stop_id()) + " has " +
                  std::to_string(nodeinfo->edge_count()));
    } else {
      bool sc = edge.shortcut();
      LOG_ERROR((boost::format("No opposing edge at LL=%1%,%2% Length = %3% Startnode %4% EndNode %5% Shortcut %6%")
      % nodeinfo->latlng().lat() % nodeinfo->latlng().lng() % edge.length()
      % startnode % edge.endnode() % sc).str());

      uint32_t n = 0;
      directededge = tile->directededge(nodeinfo->edge_index());
      for (uint32_t i = 0; i < nodeinfo->edge_count(); i++, directededge++) {
        if (sc == directededge->is_shortcut() && directededge->is_shortcut()) {
          LOG_WARN((boost::format("    Length = %1% Endnode: %2%")
          % directededge->length() % directededge->endnode()).str());
          n++;
        }
      }
      if (n == 0) {
        if (sc) {
          LOG_WARN("   No Shortcut edges found from end node");
        } else {
          LOG_WARN("   No regular edges found from end node");
        }
      }
    }
    return kMaxEdgesPerNode;
  }
  return opp_index;
}

bool IsPedestrianTerminal(GraphTileBuilder &tilebuilder, GraphReader& reader, std::mutex& lock,
                const GraphId& startnode,
                NodeInfoBuilder& startnodeinfo,
                DirectedEdgeBuilder& directededge,
                validator_stats::RouletteData& rd,
                const uint32_t idx) {

  bool is_terminal = true;
  lock.lock();
  const GraphTile* tile = reader.GetGraphTile(startnode);
  lock.unlock();
  const DirectedEdge* diredge = tile->directededge(startnodeinfo.edge_index());
  for (uint32_t i = 0; i < startnodeinfo.edge_count(); i++, diredge++) {

    if (i == idx)
      continue;

    if (((diredge->reverseaccess() & kPedestrianAccess) ||
        (diredge->forwardaccess() & kPedestrianAccess)) &&
        (!(diredge->forwardaccess() & kAutoAccess) &&
          !(diredge->reverseaccess() & kAutoAccess))){
      continue;
    }
    else {
      is_terminal = false;
      break;
    }
  }

  if (is_terminal) {
    if (startnodeinfo.edge_count() > 1) {
      rd.AddTask(startnodeinfo.latlng(),
                 tilebuilder.edgeinfo(directededge.edgeinfo_offset())->wayid(),
                 tilebuilder.edgeinfo(directededge.edgeinfo_offset())->shape());      
      return true;
    }
  }
  return false;
}

bool IsLoopTerminal(GraphTileBuilder &tilebuilder, GraphReader& reader, std::mutex& lock,
                const GraphId& startnode,
                NodeInfoBuilder& startnodeinfo,
                DirectedEdgeBuilder& directededge,
                validator_stats::RouletteData& rd) {

  lock.lock();
  const GraphTile* tile = reader.GetGraphTile(startnode);
  lock.unlock();
  const DirectedEdge* diredge = tile->directededge(startnodeinfo.edge_index());
  uint32_t inbound = 0, outbound = 0;

  for (uint32_t i = 0; i < startnodeinfo.edge_count(); i++, diredge++) {

    if (((diredge->forwardaccess() & kAutoAccess) &&
        !(diredge->reverseaccess() & kAutoAccess)) ||
        ((diredge->forwardaccess() & kAutoAccess) &&
         (diredge->reverseaccess() & kAutoAccess)))
      outbound++;

    if ((!(diredge->forwardaccess() & kAutoAccess) &&
        (diredge->reverseaccess() & kAutoAccess)) ||
        ((diredge->forwardaccess() & kAutoAccess) &&
         (diredge->reverseaccess() & kAutoAccess)))
      inbound++;
  }

  if ((outbound >= 2 && inbound == 0) || (inbound >= 2 && outbound == 0)) {
    rd.AddTask(startnodeinfo.latlng(),
               tilebuilder.edgeinfo(directededge.edgeinfo_offset())->wayid(),
               tilebuilder.edgeinfo(directededge.edgeinfo_offset())->shape());
    return true;
  }
  return false;
}

bool IsReversedOneway(GraphTileBuilder &tilebuilder, GraphReader& reader, std::mutex& lock,
                const GraphId& startnode,
                NodeInfoBuilder& startnodeinfo,
                DirectedEdgeBuilder& directededge,
                validator_stats::RouletteData& rd) {

  lock.lock();
  const GraphTile* tile = reader.GetGraphTile(startnode);
  lock.unlock();
  const DirectedEdge* diredge = tile->directededge(startnodeinfo.edge_index());
  uint32_t inbound = 0, outbound = 0;

  for (uint32_t i = 0; i < startnodeinfo.edge_count(); i++, diredge++) {

    if ((diredge->forwardaccess() & kAutoAccess) &&
        !(diredge->reverseaccess() & kAutoAccess))
      outbound++;

    if (!(diredge->forwardaccess() & kAutoAccess) &&
        (diredge->reverseaccess() & kAutoAccess))
      inbound++;
  }

  if (!outbound && inbound)
  {
    const GraphId endnode = directededge.endnode();
    lock.lock();
    const GraphTile* tile = reader.GetGraphTile(endnode);
    lock.unlock();
    const NodeInfo* nodeinfo = tile->node(endnode.id());

    const DirectedEdge* diredge = tile->directededge(nodeinfo->edge_index());
    uint32_t inbound = 0, outbound = 0;

    for (uint32_t i = 0; i < nodeinfo->edge_count(); i++, diredge++) {

      if ((diredge->forwardaccess() & kAutoAccess) &&
          !(diredge->reverseaccess() & kAutoAccess))
        outbound++;

      if (!(diredge->forwardaccess() & kAutoAccess) &&
          (diredge->reverseaccess() & kAutoAccess))
        inbound++;
    }

    if (!outbound && inbound) {
      rd.AddTask(startnodeinfo.latlng(),
                 tilebuilder.edgeinfo(directededge.edgeinfo_offset())->wayid(),
                 tilebuilder.edgeinfo(directededge.edgeinfo_offset())->shape());
      return true;
    }
  }

  return false;
}



void validate(const boost::property_tree::ptree& hierarchy_properties,
              std::queue<GraphId>& tilequeue, std::mutex& lock,
              std::promise<validator_stats>& result) {

    // Our local class for gathering the stats
    validator_stats vStats;
    // Local Graphreader
    GraphReader graph_reader(hierarchy_properties);
    // Get some things we need throughout
    auto tile_hierarchy = graph_reader.GetTileHierarchy();
    std::vector<Tiles<PointLL>> levels;
    for (auto level : tile_hierarchy.levels()) {
      levels.push_back(level.second.tiles);
    }
    Tiles<PointLL> *tiles;

    // Check for more tiles
    while (true) {
      lock.lock();
      if (tilequeue.empty()) {
        lock.unlock();
        break;
      }
      // Get the next tile Id
      GraphId tile_id = tilequeue.front();
      tilequeue.pop();
      lock.unlock();

      // Point tiles to the set we need for current level
      size_t level = tile_id.level();
      tiles = &levels[level];
      auto tileid = tile_id.tileid();

      uint32_t dupcount = 0;

      // Get the tile
      GraphTileBuilder tilebuilder(tile_hierarchy, tile_id, false);
      const GraphTile signtile(tile_hierarchy, tile_id);

      // Copy existing header. No need to update any counts or offsets.
      GraphTileHeader existinghdr = *(tilebuilder.header());
      const GraphTileHeaderBuilder hdrbuilder =
          static_cast<const GraphTileHeaderBuilder&>(existinghdr);

      // Update nodes and directed edges as needed
      std::vector<NodeInfoBuilder> nodes;
      std::vector<DirectedEdgeBuilder> directededges;

      // Iterate through the nodes and the directed edges
      float roadlength = 0.0f;
      uint32_t nodecount = tilebuilder.header()->nodecount();
      GraphId node = tile_id;
      for (uint32_t i = 0; i < nodecount; i++, node++) {
        NodeInfoBuilder nodeinfo = tilebuilder.node(i);
        const NodeInfo* signnodeinfo = signtile.node(i);

        lock.lock();
        const GraphTile* tile = graph_reader.GetGraphTile(node);
        lock.unlock();
        std::string begin_node_iso = tile->admin(nodeinfo.admin_index())->country_iso();

        // Go through directed edges and update data
        uint32_t idx = signnodeinfo->edge_index();
        for (uint32_t j = 0, n = nodeinfo.edge_count(); j < n; j++, idx++) {
          const DirectedEdge* signdirectededge = signtile.directededge(idx);
          // Validate signs
          if (signdirectededge->exitsign()) {
            if (signtile.GetSigns(idx).size() == 0) {
              LOG_ERROR("Directed edge marked as having signs but none found");
            }
          }

          DirectedEdgeBuilder& directededge = tilebuilder.directededge(
              nodeinfo.edge_index() + j);
          // Road Length and some variables for statistics
          float tempLength;
          bool validLength = false;
          if (!directededge.shortcut() && !directededge.trans_up() &&
              !directededge.trans_down()) {
            tempLength = directededge.length();
            roadlength += tempLength;
            validLength = true;
          }
          // Set the opposing edge index and get the country ISO at the
          // end node)
          std::string end_node_iso;
          directededge.set_opp_index(GetOpposingEdgeIndex(node, directededge,
                                                          graph_reader, dupcount, end_node_iso, lock));
          // Mark a country crossing if country ISO codes do not match
          if (!begin_node_iso.empty() && !end_node_iso.empty() &&
              begin_node_iso != end_node_iso)
            directededge.set_ctry_crossing(true);
          directededges.emplace_back(std::move(directededge));

          // Only consider edge if edge is good and it's not a link
          if (validLength && !directededge.link()) {

            auto rclass = directededge.classification();
            tempLength /= (tileid == directededge.endnode().tileid()) ? 2 : 4;
            //Determine access for directed edge
            auto fward = ((kAutoAccess & directededge.forwardaccess()) == kAutoAccess);
            auto bward = ((kAutoAccess & directededge.reverseaccess()) == kAutoAccess);
            // Check if one way
            if ((!fward || !bward) && (fward || bward)) {

              bool found = IsPedestrianTerminal(tilebuilder,graph_reader,lock,node,nodeinfo,directededge,vStats.roulette_data,j);

              if (!found && directededge.endnode().id() == node.id()) {

                lock.lock();
                const GraphTile* end_tile = graph_reader.GetGraphTile(directededge.endnode());
                lock.unlock();

                if (tile->id() == end_tile->id())
                  found = IsLoopTerminal(tilebuilder,graph_reader,lock,node,nodeinfo,directededge,vStats.roulette_data);
              }

              if (!found && directededge.endnode().id() != node.id()) {
                found = IsReversedOneway(tilebuilder,graph_reader,lock,node,nodeinfo,directededge,vStats.roulette_data);
              }

              vStats.add_tile_one_way(tileid, rclass, tempLength);
              vStats.add_country_one_way(begin_node_iso, rclass, tempLength);
            }
            // Check if this edge is internal
            if (directededge.internal()) {
              vStats.add_tile_int_edge(tileid, rclass);
              vStats.add_country_int_edge(begin_node_iso, rclass);
            }
            // Check if edge has maxspeed tag
            if (directededge.speed_type() == SpeedType::kTagged){
              vStats.add_tile_speed_info(tileid, rclass, tempLength);
              vStats.add_country_speed_info(begin_node_iso, rclass, tempLength);
            }
            // Check if edge has any names
            if (tilebuilder.edgeinfo(directededge.edgeinfo_offset())->name_count() > 0) {
              vStats.add_tile_named(tileid, rclass, tempLength);
              vStats.add_country_named(begin_node_iso, rclass, tempLength);
            }
            // Add road lengths to statistics for current country and tile
            vStats.add_country_road(begin_node_iso, rclass, tempLength);
            vStats.add_tile_road(tileid, rclass, tempLength);
          }
        }
        // Add the node to the list
        nodes.emplace_back(std::move(nodeinfo));
      }

      // Add density to return class. Approximate the tile area square km
      AABB2<PointLL> bb = tiles->TileBounds(tileid);
      float area = ((bb.maxy() - bb.miny()) * kMetersPerDegreeLat * kKmPerMeter) *
                   ((bb.maxx() - bb.minx()) *
                       DistanceApproximator::MetersPerLngDegree(bb.Center().y()) * kKmPerMeter);
      float density = (roadlength * 0.0005f) / area;
      vStats.add_density(density, level);
      vStats.add_tile_area(tileid, area);
      vStats.add_tile_geom(tileid, tiles->TileBounds(tileid));

      // Write the new tile
      lock.lock();
      tilebuilder.Update(tile_hierarchy, hdrbuilder, nodes, directededges);
      lock.unlock();

      // Check if we need to clear the tile cache
      lock.lock();
      if (graph_reader.OverCommitted())
        graph_reader.Clear();
      lock.unlock();

      // Add possible duplicates to return class
      vStats.add_dup(dupcount, level);
    }

    // Fill promise with statistics
    result.set_value(vStats);
  }
}

namespace valhalla {
namespace mjolnir {

  void GraphValidator::Validate(const boost::property_tree::ptree& pt) {

    // Graphreader
    boost::property_tree::ptree hierarchy_properties = (pt.get_child("mjolnir.hierarchy"));
    GraphReader reader(hierarchy_properties);
    // Make sure there are at least 2 levels!
    if (reader.GetTileHierarchy().levels().size() < 2)
      throw std::runtime_error("Bad tile hierarchy - need 2 levels");

    // Setup threads
    std::vector<std::shared_ptr<std::thread> > threads(
        std::max(static_cast<unsigned int>(1),
                 pt.get<unsigned int>("concurrency",std::thread::hardware_concurrency())));

    // Setup promises
    std::list<std::promise<validator_stats> > results;

    // Create a randomized queue of tiles to work from
    const auto tile_hierarchy = reader.GetTileHierarchy();
    std::deque<GraphId> tempqueue;
    for (auto tier : tile_hierarchy.levels()) {
      auto level = tier.second.level;
      auto tiles = tier.second.tiles;
      for (uint32_t id = 0; id < tiles.TileCount(); id++) {
        // If tile exists add it to the queue
        GraphId tile_id(id, level, 0);
        if (GraphReader::DoesTileExist(tile_hierarchy, tile_id)) {
          tempqueue.push_back(tile_id);
        }
      }
    }
    std::random_shuffle(tempqueue.begin(), tempqueue.end());
    std::queue<GraphId> tilequeue(tempqueue);

    // An mutex we can use to do the synchronization
    std::mutex lock;

    LOG_INFO("Validating signs and connectivity");
    // Spawn the threads
    for (auto& thread : threads) {
      results.emplace_back();
      thread.reset(new std::thread(validate, std::cref(hierarchy_properties),
                                   std::ref(tilequeue),
                                   std::ref(lock), std::ref(results.back())));
    }
    // Wait for threads to finish
    for (auto& thread : threads) {
      thread->join();
    }
    // Get the returned data from the promise
    validator_stats stats;
    for (auto& result : results) {
      auto data = result.get_future().get();
      stats.add(data);
    }
    // Add up total dupcount_ and find densities
    LOG_INFO("Finished");
    for (uint8_t level = 0; level <= 2; level++) {
      // Print duplicates info for level
      std::vector<uint32_t> dups = stats.get_dups(level);
      uint32_t dupcount = std::accumulate(dups.begin(), dups.end(), 0);
      LOG_WARN((boost::format("Possible duplicates at level: %1% = %2%")
      % std::to_string(level) % dupcount).str());
      // Get the average density and the max density
      float max_density = 0.0f;
      float sum = 0.0f;
      for (auto density : stats.get_densities(level)) {
        if (density > max_density) {
          max_density = density;
        }
        sum += density;
      }
      float average_density = sum / stats.get_densities(level).size();
      LOG_DEBUG("Average density = " + std::to_string(average_density) +
               " max = " + std::to_string(max_density));
    }
    stats.build_db(pt);
    stats.roulette_data.GenerateTasks();
  }
}
}
