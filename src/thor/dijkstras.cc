#include "thor/dijkstras.h"
#include "baldr/datetime.h"
#include "midgard/distanceapproximator.h"
#include "midgard/logging.h"
#include <algorithm>
#include <map>

using namespace valhalla::midgard;
using namespace valhalla::baldr;
using namespace valhalla::sif;

namespace {

// Method to get an operator Id from a map of operator strings vs. Id.
uint32_t GetOperatorId(const GraphTile* tile,
                       uint32_t routeid,
                       std::unordered_map<std::string, uint32_t>& operators) {
  const TransitRoute* transit_route = tile->GetTransitRoute(routeid);

  // Test if the transit operator changed
  if (transit_route && transit_route->op_by_onestop_id_offset()) {
    // Get the operator name and look up in the operators map
    std::string operator_name = tile->GetName(transit_route->op_by_onestop_id_offset());
    auto operator_itr = operators.find(operator_name);
    if (operator_itr == operators.end()) {
      // Operator not found - add to the map
      uint32_t id = operators.size() + 1;
      operators[operator_name] = id;
      return id;
    } else {
      return operator_itr->second;
    }
  }
  return 0;
}

} // namespace

namespace valhalla {
namespace thor {

// Default constructor
Dijkstras::Dijkstras()
    : has_date_time_(false), start_tz_index_(0), access_mode_(kAutoAccess), mode_(TravelMode::kDrive),
      adjacencylist_(nullptr) {
}

// Clear the temporary information generated during path construction.
void Dijkstras::Clear() {
  // Clear the edge labels, edge status flags, and adjacency list
  // TODO - clear only the edge label set that was used?
  bdedgelabels_.clear();
  mmedgelabels_.clear();
  adjacencylist_.reset();
  edgestatus_.clear();
}

// Initialize - create adjacency list, edgestatus support, and reserve
// edgelabels
template <typename label_container_t>
void Dijkstras::Initialize(label_container_t& labels, const uint32_t bucket_size) {
  // Set aside some space for edge labels
  uint32_t edge_label_reservation;
  uint32_t bucket_count;
  GetExpansionHints(bucket_count, edge_label_reservation);
  labels.reserve(edge_label_reservation);

  // Set up lambda to get sort costs
  const auto edgecost = [&labels](const uint32_t label) { return labels[label].sortcost(); };
  float range = bucket_count * bucket_size;
  adjacencylist_.reset(new DoubleBucketQueue(0.0f, range, bucket_size, edgecost));
}
template void
Dijkstras::Initialize<decltype(Dijkstras::bdedgelabels_)>(decltype(Dijkstras::bdedgelabels_)&,
                                                          const uint32_t);
template void
Dijkstras::Initialize<decltype(Dijkstras::mmedgelabels_)>(decltype(Dijkstras::mmedgelabels_)&,
                                                          const uint32_t);

// Initializes the time of the expansion if there is one
std::pair<uint64_t, uint32_t>
Dijkstras::SetTime(google::protobuf::RepeatedPtrField<valhalla::Location>& locations,
                   const GraphId& node_id,
                   GraphReader& reader) {

  // No time for this expansion
  const auto& location = locations.Get(0);
  has_date_time_ = false;
  if (!location.has_date_time() || !node_id.Is_Valid())
    return {};

  // Set the timezone to be the timezone at the end node
  start_tz_index_ = GetTimezone(reader, node_id);
  if (start_tz_index_ == 0)
    LOG_ERROR("Could not get the timezone at the destination location");

  // Set route start time (seconds from epoch)
  auto start_time = DateTime::seconds_since_epoch(location.date_time(),
                                                  DateTime::get_tz_db().from_index(start_tz_index_));

  // Set seconds from beginning of the week
  auto start_seconds_of_week = DateTime::day_of_week(location.date_time()) * kSecondsPerDay +
                               DateTime::seconds_from_midnight(location.date_time());
  has_date_time_ = true;

  // loop over all locations setting the date time with timezone
  for (auto& location : locations) {
    // no time skip
    if (!location.has_date_time())
      continue;
    // find a node
    for (const auto& e : location.path_edges()) {
      // get the edge and then the end node
      GraphId edge_id(e.graph_id());
      const auto* tile = reader.GetGraphTile(edge_id);
      GraphId node_id = tile ? tile->directededge(edge_id)->endnode() : GraphId{};
      if (reader.GetGraphTile(node_id, tile)) {
        // if its current time  use that otherwise use the time provided
        const auto* node = tile->node(node_id);
        auto tz_index = DateTime::get_tz_db().from_index(node->timezone());
        auto date_time =
            location.date_time() == "current"
                ? DateTime::iso_date_time(tz_index)
                : DateTime::seconds_to_date(DateTime::seconds_since_epoch(location.date_time(),
                                                                          tz_index),
                                            tz_index);
        location.set_date_time(date_time);
        break;
      }
    }
  }

  // Hand back the start time and second of the week
  return {start_time, start_seconds_of_week};
}

// Expand from a node in the forward direction
void Dijkstras::ExpandForward(GraphReader& graphreader,
                              const GraphId& node,
                              const EdgeLabel& pred,
                              const uint32_t pred_idx,
                              const bool from_transition,
                              uint64_t localtime,
                              int32_t seconds_of_week) {
  std::cout << "Expanding pred " << pred.edgeid().id() << std::endl;
  // Get the tile and the node info. Skip if tile is null (can happen
  // with regional data sets) or if no access at the node.
  const GraphTile* tile = graphreader.GetGraphTile(node);
  if (tile == nullptr) {
    return;
  }

  // Get the nodeinfo
  const NodeInfo* nodeinfo = tile->node(node);

  // We dont need to do transitions again we just need to queue the edges that leave them
  if (!from_transition) {
    // Let implementing class we are expanding from here
    EdgeLabel* prev_pred =
        pred.predecessor() == kInvalidLabel ? nullptr : &bdedgelabels_[pred.predecessor()];
    ExpandingNode(graphreader, pred, tile->get_node_ll(node), prev_pred);
  }

  // Bail if we cant expand from here
  if (!costing_->Allowed(nodeinfo)) {
    return;
  }

  // Adjust for time zone (if different from timezone at the start).
  if (nodeinfo->timezone() != start_tz_index_) {
    // Get the difference in seconds between the origin tz and current tz
    int tz_diff =
        DateTime::timezone_diff(localtime, DateTime::get_tz_db().from_index(start_tz_index_),
                                DateTime::get_tz_db().from_index(nodeinfo->timezone()));
    localtime += tz_diff;
    seconds_of_week = DateTime::normalize_seconds_of_week(seconds_of_week + tz_diff);
  }

  // Expand from end node in forward direction.
  GraphId edgeid = {node.tileid(), node.level(), nodeinfo->edge_index()};
  EdgeStatusInfo* es = edgestatus_.GetPtr(edgeid, tile);
  const DirectedEdge* directededge = tile->directededge(edgeid);
  for (uint32_t i = 0; i < nodeinfo->edge_count(); ++i, ++directededge, ++edgeid, ++es) {
    std::cout << "  Expanding edgeid " << edgeid.id() << std::endl;

    // Skip this edge if permanently labeled (best path already found to this
    // directed edge). skip shortcuts or if no access is allowed to this edge
    // (based on the costing method) or if a complex restriction exists for
    // this path.
    if (directededge->is_shortcut() || es->set() == EdgeSet::kPermanent ||
        !(directededge->forwardaccess() & access_mode_)) {
      std::cout << "    continue " << std::endl;
      continue;
    }

    // Check if the edge is allowed or if a restriction occurs
    EdgeStatus* todo = nullptr;
    bool has_time_restrictions = false;
    if (has_date_time_) {
      // With date time we check time dependent restrictions and access
      if (!costing_->Allowed(directededge, pred, tile, edgeid, localtime, nodeinfo->timezone(),
                             has_time_restrictions) ||
          costing_->Restricted(directededge, pred, bdedgelabels_, tile, edgeid, true, todo, localtime,
                               nodeinfo->timezone())) {
        std::cout << "    not allowed " << std::endl;
        continue;
      }
    } else {
      if (!costing_->Allowed(directededge, pred, tile, edgeid, 0, 0, has_time_restrictions) ||
          costing_->Restricted(directededge, pred, bdedgelabels_, tile, edgeid, true)) {
        std::cout << "    not allowed " << std::endl;
        continue;
      }
    }

    // Compute the cost to the end of this edge
    Cost transition_cost = costing_->TransitionCost(directededge, nodeinfo, pred);
    Cost newcost =
        pred.cost() +
        costing_->EdgeCost(directededge, tile,
                           has_date_time_ ? seconds_of_week : kConstrainedFlowSecondOfDay) +
        transition_cost;

    // Check if edge is temporarily labeled and this path has less cost. If
    // less cost the predecessor is updated and the sort cost is decremented
    // by the difference in real cost (A* heuristic doesn't change)
    if (es->set() == EdgeSet::kTemporary) {
      EdgeLabel& lab = bdedgelabels_[es->index()];
      if (newcost.cost < lab.cost().cost) {
        float newsortcost = lab.sortcost() - (lab.cost().cost - newcost.cost);
        adjacencylist_->decrease(es->index(), newsortcost);
        lab.Update(pred_idx, newcost, newsortcost, transition_cost, has_time_restrictions);
      }
      std::cout << "    updated " << std::endl;
      continue;
    }

    // Only needed if you want to connect with a reverse path
    const GraphTile* t2 = tile;
    GraphId oppedgeid = graphreader.GetOpposingEdgeId(edgeid, t2);

    // Add edge label, add to the adjacency list and set edge status
    uint32_t idx = bdedgelabels_.size();
    *es = {EdgeSet::kTemporary, idx};
    bdedgelabels_.emplace_back(pred_idx, edgeid, oppedgeid, directededge, newcost, newcost.cost, 0.0f,
                               mode_, transition_cost, false, has_time_restrictions);
    adjacencylist_->add(idx);
    std::cout << "    emplaced " << std::endl;
  }

  // Handle transitions - expand from the end node of each transition
  if (!from_transition && nodeinfo->transition_count() > 0) {
    const NodeTransition* trans = tile->transition(nodeinfo->transition_index());
    for (uint32_t i = 0; i < nodeinfo->transition_count(); ++i, ++trans) {
      ExpandForward(graphreader, trans->endnode(), pred, pred_idx, true, localtime, seconds_of_week);
    }
  }
}

// Compute the forward graph traversal
void Dijkstras::Compute(google::protobuf::RepeatedPtrField<valhalla::Location>& origin_locations,
                        GraphReader& graphreader,
                        const std::shared_ptr<DynamicCost>* mode_costing,
                        const TravelMode mode) {
  LOGLN_WARN("COMPUTE");

  // Set the mode and costing
  mode_ = mode;
  costing_ = mode_costing[static_cast<uint32_t>(mode_)];
  access_mode_ = costing_->access_mode();

  // Prepare for a graph traversal
  Initialize(bdedgelabels_, costing_->UnitSize());
  SetOriginLocations(graphreader, origin_locations, costing_);

  // Check if date_time is set on the origin location. Set the seconds_of_week if it is set
  uint64_t start_time;
  uint32_t start_seconds_of_week;
  auto node_id = bdedgelabels_.empty() ? GraphId{} : bdedgelabels_[0].endnode();
  std::tie(start_time, start_seconds_of_week) = SetTime(origin_locations, node_id, graphreader);

  // Compute the isotile
  auto cb_decision = ExpansionRecommendation::continue_expansion;
  while (cb_decision != ExpansionRecommendation::stop_expansion) {
    // Get next element from adjacency list. Check that it is valid. An
    // invalid label indicates there are no edges that can be expanded.
    uint32_t predindex = adjacencylist_->pop();
    if (predindex == kInvalidLabel) {
      break;
    }

    // Copy the EdgeLabel for use in costing and settle the edge.
    EdgeLabel pred = bdedgelabels_[predindex];
    edgestatus_.Update(pred.edgeid(), EdgeSet::kPermanent);

    // Update local time and seconds from beginning of the week
    uint64_t localtime = start_time + static_cast<uint32_t>(pred.cost().secs);
    int32_t seconds_of_week = start_seconds_of_week + static_cast<uint32_t>(pred.cost().secs);
    if (seconds_of_week > midgard::kSecondsPerWeek) {
      seconds_of_week -= midgard::kSecondsPerWeek;
    }

    // Check if we should stop
    cb_decision = ShouldExpand(graphreader, pred, InfoRoutingType::forward);
    if (cb_decision != ExpansionRecommendation::prune_expansion) {
      // Expand from the end node in forward direction.
      ExpandForward(graphreader, pred.endnode(), pred, predindex, false, localtime, seconds_of_week);
    }
  }
  LOGLN_WARN("COMPUTE finished");
  for (auto label : bdedgelabels_) {
    std::cout << label.edgeid().id() << " ";
  }
  std::cout << std::endl;
}

// Expand from a node in reverse direction.
void Dijkstras::ExpandReverse(GraphReader& graphreader,
                              const GraphId& node,
                              const BDEdgeLabel& pred,
                              const uint32_t pred_idx,
                              const DirectedEdge* opp_pred_edge,
                              const bool from_transition,
                              uint64_t localtime,
                              int32_t seconds_of_week) {
  std::cout << "Expanding pred " << pred.edgeid().id() << " node "<<node.id() << std::endl;
  // << " opp_pred "<< opp_pred_edge.edgeid().id()<< std::endl;
  // Get the tile and the node info. Skip if tile is null (can happen
  // with regional data sets) or if no access at the node.
  const GraphTile* tile = graphreader.GetGraphTile(node);
  if (tile == nullptr) {
    return;
  }

  // Get the nodeinfo
  const NodeInfo* nodeinfo = tile->node(node);

  // We dont need to do transitions again we just need to queue the edges that leave them
  if (!from_transition) {
    // Let implementing class we are expanding from here
    EdgeLabel* prev_pred =
        pred.predecessor() == kInvalidLabel ? nullptr : &bdedgelabels_[pred.predecessor()];
    ExpandingNode(graphreader, pred, tile->get_node_ll(node), prev_pred);
  }

  // Bail if we cant expand from here
  if (!costing_->Allowed(nodeinfo)) {
    return;
  }

  // Adjust for time zone (if different from timezone at the start).
  if (nodeinfo->timezone() != start_tz_index_) {
    // Get the difference in seconds between the origin tz and current tz
    int tz_diff =
        DateTime::timezone_diff(localtime, DateTime::get_tz_db().from_index(start_tz_index_),
                                DateTime::get_tz_db().from_index(nodeinfo->timezone()));
    localtime += tz_diff;
    seconds_of_week = DateTime::normalize_seconds_of_week(seconds_of_week + tz_diff);
  }

  // Expand from end node in reverse direction.
  GraphId edgeid = {node.tileid(), node.level(), nodeinfo->edge_index()};
  EdgeStatusInfo* es = edgestatus_.GetPtr(edgeid, tile);
  const DirectedEdge* directededge = tile->directededge(edgeid);
  for (uint32_t i = 0; i < nodeinfo->edge_count(); ++i, ++directededge, ++edgeid, ++es) {
    std::cout << "  Expanding edge " << edgeid.id() << std::endl;
    // Skip this edge if permanently labeled (best path already found to this
    // directed edge), if no access for this mode, or if edge is a shortcut
    if (!(directededge->reverseaccess() & access_mode_) || directededge->is_shortcut() ||
        es->set() == EdgeSet::kPermanent) {
      std::cout << "    continue " << std::endl;
      continue;
    }

    // Get end node tile, opposing edge Id, and opposing directed edge.
    const GraphTile* t2 = tile;
    auto opp_edge_id = graphreader.GetOpposingEdgeId(edgeid, t2);
    if (t2 == nullptr) {
      continue;
    }
    const DirectedEdge* opp_edge = t2->directededge(opp_edge_id);

    // Check if the edge is allowed or if a restriction occurs
    EdgeStatus* todo = nullptr;
    bool has_time_restrictions = false;
    if (has_date_time_) {
      // With date time we check time dependent restrictions and access
      if (!costing_->AllowedReverse(directededge, pred, opp_edge, t2, opp_edge_id, localtime,
                                    nodeinfo->timezone(), has_time_restrictions) ||
          costing_->Restricted(directededge, pred, bdedgelabels_, tile, edgeid, false, todo,
                               localtime, nodeinfo->timezone())) {
        std::cout << "    not allowed " << std::endl;
        continue;
      }
    } else {
      if (!costing_->AllowedReverse(directededge, pred, opp_edge, t2, opp_edge_id, 0, 0,
                                    has_time_restrictions) ||
          costing_->Restricted(directededge, pred, bdedgelabels_, tile, edgeid, false)) {
        std::cout << "    not allowed " << std::endl;
        continue;
      }
    }

    // Compute the cost to the end of this edge with separate transition cost
    Cost transition_cost = costing_->TransitionCostReverse(directededge->localedgeidx(), nodeinfo,
                                                           opp_edge, opp_pred_edge);
    Cost newcost = pred.cost() +
                   costing_->EdgeCost(opp_edge, t2,
                                      has_date_time_ ? seconds_of_week : kConstrainedFlowSecondOfDay);
    newcost.cost += transition_cost.cost;

    // Check if edge is temporarily labeled and this path has less cost. If
    // less cost the predecessor is updated and the sort cost is decremented
    // by the difference in real cost (A* heuristic doesn't change)
    if (es->set() == EdgeSet::kTemporary) {
      BDEdgeLabel& lab = bdedgelabels_[es->index()];
      if (newcost.cost < lab.cost().cost) {
        float newsortcost = lab.sortcost() - (lab.cost().cost - newcost.cost);
        adjacencylist_->decrease(es->index(), newsortcost);
        lab.Update(pred_idx, newcost, newsortcost, transition_cost, has_time_restrictions);
      }
      std::cout << "    updated " << std::endl;
      continue;
    }

    // Add edge label, add to the adjacency list and set edge status
    uint32_t idx = bdedgelabels_.size();
    *es = {EdgeSet::kTemporary, idx};
    bdedgelabels_.emplace_back(pred_idx, edgeid, opp_edge_id, directededge, newcost, newcost.cost,
                               0.0f, mode_, transition_cost, false, has_time_restrictions);
    adjacencylist_->add(idx);
    std::cout << "    emplaced " << std::endl;
  }

  // Handle transitions - expand from the end node of each transition
  if (!from_transition && nodeinfo->transition_count() > 0) {
    const NodeTransition* trans = tile->transition(nodeinfo->transition_index());
    for (uint32_t i = 0; i < nodeinfo->transition_count(); ++i, ++trans) {
      ExpandReverse(graphreader, trans->endnode(), pred, pred_idx, opp_pred_edge, true, localtime,
                    seconds_of_week);
    }
  }
}

// Compute the reverse graph traversal
void Dijkstras::ComputeReverse(google::protobuf::RepeatedPtrField<valhalla::Location>& dest_locations,
                               GraphReader& graphreader,
                               const std::shared_ptr<DynamicCost>* mode_costing,
                               const TravelMode mode) {
  LOGLN_WARN("COMPUTEREVERSE");
  for (auto& location : dest_locations) {

    std::cout << "dest_locations: loc "<<std::endl;
    for (auto& edge : location.path_edges()) {
      std::cout << "  edge "<<GraphId(edge.graph_id()).id()<< " endnode "<<GraphId(edge.end_node()).id();
    }
    std::cout<<std::endl;
  }
  // Set the mode and costing
  mode_ = mode;
  costing_ = mode_costing[static_cast<uint32_t>(mode_)];
  access_mode_ = costing_->access_mode();

  // Prepare for graph traversal
  Initialize(bdedgelabels_, costing_->UnitSize());
  SetDestinationLocations(graphreader, dest_locations, costing_);

  // Check if date_time is set on the destination location. Set the seconds_of_week if it is set
  uint64_t start_time;
  uint32_t start_seconds_of_week;
  auto node_id = bdedgelabels_.empty() ? GraphId{} : bdedgelabels_[0].endnode();
  std::tie(start_time, start_seconds_of_week) = SetTime(dest_locations, node_id, graphreader);

  // Compute the isotile
  auto cb_decision = ExpansionRecommendation::continue_expansion;
  while (cb_decision != ExpansionRecommendation::stop_expansion) {
    // Get next element from adjacency list. Check that it is valid. An
    // invalid label indicates there are no edges that can be expanded.
    uint32_t predindex = adjacencylist_->pop();
    if (predindex == kInvalidLabel) {
      break;
    }

    // Copy the EdgeLabel for use in costing and settle the edge.
    BDEdgeLabel pred = bdedgelabels_[predindex];
    edgestatus_.Update(pred.edgeid(), EdgeSet::kPermanent);

    // Get the opposing predecessor directed edge. Need to make sure we get
    // the correct one if a transition occurred
    const DirectedEdge* opp_pred_edge =
        graphreader.GetGraphTile(pred.opp_edgeid())->directededge(pred.opp_edgeid());

    // Update local time and seconds from beginning of the week
    uint64_t localtime = start_time + static_cast<uint32_t>(pred.cost().secs);
    int32_t seconds_of_week = DateTime::normalize_seconds_of_week(
        start_seconds_of_week - static_cast<uint32_t>(pred.cost().secs));

    // Check if we should stop
    cb_decision = ShouldExpand(graphreader, pred, InfoRoutingType::forward);
    if (cb_decision != ExpansionRecommendation::prune_expansion) {
      // Expand from the end node in forward direction.
      ExpandReverse(graphreader, pred.endnode(), pred, predindex, opp_pred_edge, false, localtime,
                    seconds_of_week);
    }
  }
  LOGLN_WARN("COMPUTEREVERSE finished");
  std::cout << "with " << bdedgelabels_.size() << " edges: ";
  for (auto label : bdedgelabels_) {
    std::cout << label.edgeid().id() << " ";
  }
  std::cout << std::endl;
}

// Expand from a node in forward direction using multimodal.
void Dijkstras::ExpandForwardMultiModal(GraphReader& graphreader,
                                        const GraphId& node,
                                        const MMEdgeLabel& pred,
                                        const uint32_t pred_idx,
                                        const bool from_transition,
                                        const std::shared_ptr<DynamicCost>& pc,
                                        const std::shared_ptr<DynamicCost>& tc,
                                        const std::shared_ptr<DynamicCost>* mode_costing) {
  // Get the tile and the node info. Skip if tile is null (can happen
  // with regional data sets) or if no access at the node.
  const GraphTile* tile = graphreader.GetGraphTile(node);
  if (tile == nullptr) {
    return;
  }

  // Get the nodeinfo
  const NodeInfo* nodeinfo = tile->node(node);

  // We dont need to do transitions again we just need to queue the edges that leave them
  if (!from_transition) {
    // Let implementing class we are expanding from here
    EdgeLabel* prev_pred =
        pred.predecessor() == kInvalidLabel ? nullptr : &mmedgelabels_[pred.predecessor()];
    ExpandingNode(graphreader, pred, tile->get_node_ll(node), prev_pred);
  }

  // Bail if we cant expand from here
  if (!mode_costing[static_cast<uint8_t>(mode_)]->Allowed(nodeinfo)) {
    return;
  }

  // Set local time and adjust for time zone  (if different from timezone at the start).
  uint32_t localtime = start_time_ + pred.cost().secs;
  if (nodeinfo->timezone() != start_tz_index_) {
    // Get the difference in seconds between the origin tz and current tz
    localtime += DateTime::timezone_diff(localtime, DateTime::get_tz_db().from_index(start_tz_index_),
                                         DateTime::get_tz_db().from_index(nodeinfo->timezone()));
  }

  // Set a default transfer penalty at a stop (if not same trip Id and block Id)
  Cost transfer_cost = tc->DefaultTransferCost();

  // Get any transfer times and penalties if this is a transit stop (and
  // transit has been taken at some point on the path) and mode is pedestrian
  mode_ = pred.mode();
  bool has_transit = pred.has_transit();
  GraphId prior_stop = pred.prior_stopid();
  uint32_t operator_id = pred.transit_operator();
  if (nodeinfo->type() == NodeType::kMultiUseTransitPlatform) {
    // Get the transfer penalty when changing stations
    if (mode_ == TravelMode::kPedestrian && prior_stop.Is_Valid() && has_transit) {
      transfer_cost = tc->TransferCost();
    }

    if (processed_tiles_.find(tile->id().tileid()) == processed_tiles_.end()) {
      tc->AddToExcludeList(tile);
      processed_tiles_.emplace(tile->id().tileid());
    }

    // check if excluded.
    if (tc->IsExcluded(tile, nodeinfo)) {
      return;
    }

    // Add transfer time to the local time when entering a stop as a pedestrian. This
    // is a small added cost on top of any costs along paths and roads
    if (mode_ == TravelMode::kPedestrian) {
      localtime += transfer_cost.secs;
    }

    // Update prior stop. TODO - parent/child stop info?
    prior_stop = node;

    // we must get the date from level 3 transit tiles and not level 2.  The level 3 date is
    // set when the fetcher grabbed the transit data and created the schedules.
    if (!date_set_) {
      date_ = DateTime::days_from_pivot_date(DateTime::get_formatted_date(origin_date_time_));
      dow_ = DateTime::day_of_week_mask(origin_date_time_);
      uint32_t date_created = tile->header()->date_created();
      if (date_ < date_created) {
        date_before_tile_ = true;
      } else {
        day_ = date_ - date_created;
      }
      date_set_ = true;
    }
  }

  // TODO: allow mode changes at special nodes
  //      bike share (pedestrian <--> bicycle)
  //      parking (drive <--> pedestrian)
  //      transit stop (pedestrian <--> transit).
  bool mode_change = false;

  // Expand from end node.
  GraphId edgeid(node.tileid(), node.level(), nodeinfo->edge_index());
  EdgeStatusInfo* es = edgestatus_.GetPtr(edgeid, tile);
  const DirectedEdge* directededge = tile->directededge(nodeinfo->edge_index());
  for (uint32_t i = 0; i < nodeinfo->edge_count(); i++, directededge++, ++edgeid, ++es) {
    // Skip shortcut edges and edges that are permanently labeled (best
    // path already found to this directed edge).
    if (directededge->is_shortcut() || es->set() == EdgeSet::kPermanent) {
      continue;
    }

    // Reset cost and walking distance
    Cost newcost = pred.cost();
    uint32_t walking_distance = pred.path_distance();

    // If this is a transit edge - get the next departure. Do not check if allowed by
    // costing - assume if you get a transit edge you walked to the transit stop
    uint32_t tripid = 0;
    uint32_t blockid = 0;
    bool has_time_restrictions = false;
    if (directededge->IsTransitLine()) {
      // Check if transit costing allows this edge
      if (!tc->Allowed(directededge, pred, tile, edgeid, 0, 0, has_time_restrictions)) {
        continue;
      }

      // check if excluded.
      if (tc->IsExcluded(tile, directededge)) {
        continue;
      }

      // Look up the next departure along this edge
      const TransitDeparture* departure =
          tile->GetNextDeparture(directededge->lineid(), localtime, day_, dow_, date_before_tile_,
                                 tc->wheelchair(), tc->bicycle());
      if (departure) {
        // Check if there has been a mode change
        mode_change = (mode_ == TravelMode::kPedestrian);

        // Update trip Id and block Id
        tripid = departure->tripid();
        blockid = departure->blockid();
        has_transit = true;

        // There is no cost to remain on the same trip or valid blockId
        if (tripid == pred.tripid() || (blockid != 0 && blockid == pred.blockid())) {
          // This departure is valid without any added cost. Operator Id
          // is the same as the predecessor
          operator_id = pred.transit_operator();
        } else {
          if (pred.tripid() > 0) {
            // tripId > 0 means the prior edge was a transit edge and this
            // is an "in-station" transfer. Add a small transfer time and
            // call GetNextDeparture again if we cannot make the current
            // departure.
            // TODO - is there a better way?
            if (localtime + 30 > departure->departure_time()) {
              departure = tile->GetNextDeparture(directededge->lineid(), localtime + 30, day_, dow_,
                                                 date_before_tile_, tc->wheelchair(), tc->bicycle());
              if (!departure) {
                continue;
              }
            }
          }

          // Get the operator Id
          operator_id = GetOperatorId(tile, departure->routeid(), operators_);

          // Add transfer penalty and operator change penalty
          if (pred.transit_operator() > 0 && pred.transit_operator() != operator_id) {
            // TODO - create a configurable operator change penalty
            newcost.cost += 300;
          } else {
            newcost.cost += transfer_cost.cost;
          }
        }

        // Change mode and costing to transit. Add edge cost.
        mode_ = TravelMode::kPublicTransit;
        newcost += tc->EdgeCost(directededge, departure, localtime);
      } else {
        // No matching departures found for this edge
        continue;
      }
    } // This is not a transit edge
    else {
      // If current mode is public transit we should only connect to
      // transit connection edges or transit edges
      if (mode_ == TravelMode::kPublicTransit) {
        // Disembark from transit and reset walking distance
        mode_ = TravelMode::kPedestrian;
        walking_distance = 0;
        mode_change = true;
      }

      // Regular edge - use the appropriate costing and check if access
      // is allowed. If mode is pedestrian this will validate walking
      // distance has not been exceeded.
      if (!mode_costing[static_cast<uint32_t>(mode_)]->Allowed(directededge, pred, tile, edgeid, 0, 0,
                                                               has_time_restrictions)) {
        continue;
      }

      Cost c = mode_costing[static_cast<uint32_t>(mode_)]->EdgeCost(directededge, tile);
      c.cost *= mode_costing[static_cast<uint32_t>(mode_)]->GetModeFactor();
      newcost += c;

      // Add to walking distance
      if (mode_ == TravelMode::kPedestrian) {
        walking_distance += directededge->length();

        // Prevent going from one egress connection directly to another
        // at a transit stop - this is like entering a station and exiting
        // without getting on transit
        if (nodeinfo->type() == NodeType::kTransitEgress && pred.use() == Use::kEgressConnection &&
            directededge->use() == Use::kEgressConnection) {
          continue;
        }
      }
    }

    // Add mode change cost or edge transition cost from the costing model
    Cost transition_cost{};
    if (mode_change) {
      // TODO: make mode change cost configurable. No cost for entering
      // a transit line (assume the wait time is the cost)
      // transition_cost = { 10.0f, 10.0f };
    } else {
      transition_cost =
          mode_costing[static_cast<uint32_t>(mode_)]->TransitionCost(directededge, nodeinfo, pred);
    }
    newcost += transition_cost;

    // Prohibit entering the same station as the prior.
    if (directededge->use() == Use::kTransitConnection &&
        directededge->endnode() == pred.prior_stopid()) {
      continue;
    }

    // Test if exceeding maximum transfer walking distance
    if (directededge->use() == Use::kTransitConnection && pred.prior_stopid().Is_Valid() &&
        walking_distance > max_transfer_distance_) {
      continue;
    }

    // Make the label in advance, we may not end up using it but we need it for the expansion decision
    MMEdgeLabel edge_label{pred_idx,    edgeid,           directededge,
                           newcost,     newcost.cost,     0.0f,
                           mode_,       walking_distance, tripid,
                           prior_stop,  blockid,          operator_id,
                           has_transit, transition_cost,  has_time_restrictions};

    // See if this is even worth expanding
    auto maybe_expand = ShouldExpand(graphreader, edge_label, InfoRoutingType::multi_modal);
    if (maybe_expand == ExpansionRecommendation::prune_expansion ||
        maybe_expand == ExpansionRecommendation::stop_expansion) {
      continue;
    }

    // Check if edge is temporarily labeled and this path has less cost. If
    // less cost the predecessor is updated and the sort cost is decremented
    // by the difference in real cost (A* heuristic doesn't change). Update
    // trip Id and block Id.
    if (es->set() == EdgeSet::kTemporary) {
      MMEdgeLabel& lab = mmedgelabels_[es->index()];
      if (newcost.cost < lab.cost().cost) {
        float newsortcost = lab.sortcost() - (lab.cost().cost - newcost.cost);
        adjacencylist_->decrease(es->index(), newsortcost);
        lab.Update(pred_idx, newcost, newsortcost, walking_distance, tripid, blockid, transition_cost,
                   has_time_restrictions);
      }
      continue;
    }

    // Add edge label, add to the adjacency list and set edge status
    uint32_t idx = mmedgelabels_.size();
    *es = {EdgeSet::kTemporary, idx};
    mmedgelabels_.emplace_back(std::move(edge_label));
    adjacencylist_->add(idx);
  }

  // Handle transitions - expand from the end node of each transition
  if (!from_transition && nodeinfo->transition_count() > 0) {
    const NodeTransition* trans = tile->transition(nodeinfo->transition_index());
    for (uint32_t i = 0; i < nodeinfo->transition_count(); ++i, ++trans) {
      ExpandForwardMultiModal(graphreader, trans->endnode(), pred, pred_idx, true, pc, tc,
                              mode_costing);
    }
  }
}

// Compute the reverse graph traversal for multimodal
void Dijkstras::ComputeMultiModal(
    google::protobuf::RepeatedPtrField<valhalla::Location>& origin_locations,
    GraphReader& graphreader,
    const std::shared_ptr<DynamicCost>* mode_costing,
    const TravelMode mode) {
  // For pedestrian costing - set flag allowing use of transit connections
  // Set pedestrian costing to use max distance. TODO - need for other modes
  const auto& pc = mode_costing[static_cast<uint8_t>(TravelMode::kPedestrian)];
  pc->SetAllowTransitConnections(true);
  pc->UseMaxMultiModalDistance();

  // Set the mode from the origin
  mode_ = mode;
  const auto& tc = mode_costing[static_cast<uint8_t>(TravelMode::kPublicTransit)];

  // Get maximum transfer distance
  // TODO - want to allow unlimited walking once you get off the transit stop...
  max_transfer_distance_ = 99999.0f; // costing->GetMaxTransferDistanceMM();

  // Prepare for graph traversal
  Initialize(mmedgelabels_, mode_costing[static_cast<uint8_t>(mode_)]->UnitSize());
  SetOriginLocationsMultiModal(graphreader, origin_locations,
                               mode_costing[static_cast<uint8_t>(mode_)]);

  // For now the date_time must be set on the origin.
  if (!origin_locations.Get(0).has_date_time()) {
    LOG_ERROR("No date time set on the origin location");
    return;
  }

  // Update start time
  date_set_ = false;
  date_before_tile_ = false;
  if (origin_locations.Get(0).has_date_time()) {
    // Set the timezone to be the timezone at the end node
    start_tz_index_ =
        mmedgelabels_.size() == 0 ? 0 : GetTimezone(graphreader, mmedgelabels_[0].endnode());
    if (start_tz_index_ == 0) {
      // TODO - should we throw an exception and return an error
      LOG_ERROR("Could not get the timezone at the origin location");
    }
    origin_date_time_ = origin_locations.Get(0).date_time();

    // Set route start time (seconds from midnight), date, and day of week
    start_time_ = DateTime::seconds_from_midnight(origin_locations.Get(0).date_time());
  }

  // Clear operators and processed tiles
  operators_.clear();
  processed_tiles_.clear();

  // Expand using adjacency list until we exceed threshold
  const GraphTile* tile;
  auto cb_decision = ExpansionRecommendation::continue_expansion;
  while (cb_decision != ExpansionRecommendation::stop_expansion) {
    // Get next element from adjacency list. Check that it is valid. An
    // invalid label indicates there are no edges that can be expanded.
    uint32_t predindex = adjacencylist_->pop();
    if (predindex == kInvalidLabel) {
      break;
    }

    // Copy the EdgeLabel for use in costing and settle the edge.
    MMEdgeLabel pred = mmedgelabels_[predindex];
    edgestatus_.Update(pred.edgeid(), EdgeSet::kPermanent);

    // Check if we should stop
    cb_decision = ShouldExpand(graphreader, pred, InfoRoutingType::multi_modal);
    if (cb_decision != ExpansionRecommendation::prune_expansion) {
      // Expand from the end node of the predecessor edge.
      ExpandForwardMultiModal(graphreader, pred.endnode(), pred, predindex, false, pc, tc,
                              mode_costing);
    }
  }
}

// Add edge(s) at each origin to the adjacency list
void Dijkstras::SetOriginLocations(GraphReader& graphreader,
                                   google::protobuf::RepeatedPtrField<valhalla::Location>& locations,
                                   const std::shared_ptr<DynamicCost>& costing) {
  // Add edges for each location to the adjacency list
  for (auto& location : locations) {

    // Only skip inbound edges if we have other options
    bool has_other_edges = false;
    std::for_each(location.path_edges().begin(), location.path_edges().end(),
                  [&has_other_edges](const valhalla::Location::PathEdge& e) {
                    has_other_edges = has_other_edges || !e.end_node();
                  });

    // Iterate through edges and add to adjacency list
    for (const auto& edge : (location.path_edges())) {
      // If origin is at a node - skip any inbound edge (dist = 1)
      if (has_other_edges && edge.end_node()) {
        continue;
      }

      // Disallow any user avoid edges if the avoid location is ahead of the origin along the edge
      GraphId edgeid(edge.graph_id());
      if (costing_->AvoidAsOriginEdge(edgeid, edge.percent_along())) {
        continue;
      }

      // Get the directed edge
      const GraphTile* tile = graphreader.GetGraphTile(edgeid);
      const DirectedEdge* directededge = tile->directededge(edgeid);

      // Get the opposing directed edge, continue if we cannot get it
      const GraphTile* opp_tile = nullptr;
      GraphId opp_edge_id = graphreader.GetOpposingEdgeId(edgeid, opp_tile);
      if (!opp_edge_id.Is_Valid()) {
        continue;
      }
      const DirectedEdge* opp_dir_edge = opp_tile->directededge(edgeid);

      // Get cost
      Cost cost = costing->EdgeCost(directededge, tile) * (1.0f - edge.percent_along());

      // We need to penalize this location based on its score (distance in meters from input)
      // We assume the slowest speed you could travel to cover that distance to start/end the route
      // TODO: high edge scores cause issues as there is code to limit cost so
      // that large penalties (e.g., ferries) are excluded.
      cost.cost += edge.distance() * 0.005f;

      // Construct the edge label. Set the predecessor edge index to invalid
      // to indicate the origin of the path.
      uint32_t idx = bdedgelabels_.size();
      const bool has_time_restrictions = false;
      bdedgelabels_.emplace_back(kInvalidLabel, edgeid, opp_edge_id, directededge, cost, cost.cost,
                                 0., mode_, Cost{}, false, has_time_restrictions);
      // Set the origin flag
      bdedgelabels_.back().set_origin();

      // Add EdgeLabel to the adjacency list
      adjacencylist_->add(idx);
      edgestatus_.Set(edgeid, EdgeSet::kTemporary, idx, tile);
    }
  }
}

// Add destination edges to the reverse path adjacency list.
void Dijkstras::SetDestinationLocations(
    GraphReader& graphreader,
    google::protobuf::RepeatedPtrField<valhalla::Location>& locations,
    const std::shared_ptr<DynamicCost>& costing) {
  // Add edges for each location to the adjacency list
  for (auto& location : locations) {
    // Only skip outbound edges if we have other options
    bool has_other_edges = false;
    std::for_each(location.path_edges().begin(), location.path_edges().end(),
                  [&has_other_edges](const valhalla::Location::PathEdge& e) {
                    has_other_edges = has_other_edges || !e.begin_node();
                  });

    // Iterate through edges and add to adjacency list
    for (const auto& edge : (location.path_edges())) {
      std::cout <<" SetDestinationLocations: edge: "<<GraphId(edge.graph_id()).id()<<std::endl;
      // If the destination is at a node, skip any outbound edges (so any
      // opposing inbound edges are not considered)
      if (has_other_edges && edge.begin_node()) {
        continue;
      }

      // Disallow any user avoid edges if the avoid location is ahead of the origin along the edge
      GraphId edgeid(edge.graph_id());
      if (costing_->AvoidAsDestinationEdge(edgeid, edge.percent_along())) {
        continue;
      }

      // Get the directed edge
      const GraphTile* tile = graphreader.GetGraphTile(edgeid);
      const DirectedEdge* directededge = tile->directededge(edgeid);

      // Get the opposing directed edge, continue if we cannot get it
      const GraphTile* opp_tile = nullptr;
      GraphId opp_edge_id = graphreader.GetOpposingEdgeId(edgeid, opp_tile);
      if (!opp_edge_id.Is_Valid()) {
        continue;
      }
      const DirectedEdge* opp_dir_edge = opp_tile->directededge(opp_edge_id);

      // Get the cost
      Cost cost = costing->EdgeCost(directededge, tile) * edge.percent_along();

      // We need to penalize this location based on its score (distance in meters from input)
      // We assume the slowest speed you could travel to cover that distance to start/end the route
      // TODO: high edge scores cause issues as there is code to limit cost so
      // that large penalties (e.g., ferries) are excluded.
      cost.cost += edge.distance() * 0.005f;

      // Add EdgeLabel to the adjacency list. Set the predecessor edge index
      // to invalid to indicate the origin of the path. Make sure the opposing
      // edge (edgeid) is set.
      uint32_t idx = bdedgelabels_.size();
      const bool has_time_restrictions = false;
    std::cout << "  SetOrigin: Emplacing edge_id " << edgeid.id() << " opp_edge_id "
              << opp_edge_id.id() << " end-node "<<opp_dir_edge->endnode().id() << std::endl;
      bdedgelabels_.emplace_back(kInvalidLabel, opp_edge_id, edgeid, opp_dir_edge, cost, cost.cost,
                                 0., mode_, Cost{}, false, has_time_restrictions);
      adjacencylist_->add(idx);
      edgestatus_.Set(opp_edge_id, EdgeSet::kTemporary, idx, graphreader.GetGraphTile(opp_edge_id));
    }
  }
}

// Add edge(s) at each origin to the adjacency list
void Dijkstras::SetOriginLocationsMultiModal(
    GraphReader& graphreader,
    google::protobuf::RepeatedPtrField<valhalla::Location>& origin_locations,
    const std::shared_ptr<DynamicCost>& costing) {
  // Add edges for each location to the adjacency list
  for (auto& origin : origin_locations) {
    // Only skip inbound edges if we have other options
    bool has_other_edges = false;
    std::for_each(origin.path_edges().begin(), origin.path_edges().end(),
                  [&has_other_edges](const valhalla::Location::PathEdge& e) {
                    has_other_edges = has_other_edges || !e.end_node();
                  });

    // Iterate through edges and add to adjacency list
    for (const auto& edge : (origin.path_edges())) {
      // If origin is at a node - skip any inbound edge (dist = 1)
      if (has_other_edges && edge.end_node()) {
        continue;
      }

      // Disallow any user avoid edges if the avoid location is ahead of the origin along the edge
      GraphId edgeid(edge.graph_id());
      if (costing_->AvoidAsOriginEdge(edgeid, edge.percent_along())) {
        continue;
      }

      // Get the directed edge
      const GraphTile* tile = graphreader.GetGraphTile(edgeid);
      const DirectedEdge* directededge = tile->directededge(edgeid);

      // Get the tile at the end node. Skip if tile not found as we won't be
      // able to expand from this origin edge.
      const GraphTile* endtile = graphreader.GetGraphTile(directededge->endnode());
      if (endtile == nullptr) {
        continue;
      }

      // Get cost
      Cost cost = costing->EdgeCost(directededge, endtile) * (1.0f - edge.percent_along());

      // We need to penalize this location based on its score (distance in meters from input)
      // We assume the slowest speed you could travel to cover that distance to start/end the route
      // TODO: high edge scores cause issues as there is code to limit cost so
      // that large penalties (e.g., ferries) are excluded.
      cost.cost += edge.distance() * 0.005f;

      // Add EdgeLabel to the adjacency list (but do not set its status).
      // Set the predecessor edge index to invalid to indicate the origin
      // of the path.
      uint32_t idx = mmedgelabels_.size();
      uint32_t d = static_cast<uint32_t>(directededge->length() * (1.0f - edge.percent_along()));
      // TODO Do we care about time restrictions at origin edges?
      bool has_time_restrictions = false;
      // TODO How about transition cost?
      auto transition_cost = Cost{};
      MMEdgeLabel edge_label(kInvalidLabel, edgeid, directededge, cost, cost.cost, 0.0f, mode_, d, 0,
                             GraphId(), 0, 0, false, transition_cost, has_time_restrictions);

      // Set the origin flag
      edge_label.set_origin();

      // Add EdgeLabel to the adjacency list
      mmedgelabels_.push_back(std::move(edge_label));
      adjacencylist_->add(idx);
    }
  }
}

} // namespace thor
} // namespace valhalla
