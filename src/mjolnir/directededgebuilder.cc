#include "mjolnir/directededgebuilder.h"
#include <valhalla/midgard/logging.h>

#include <algorithm>

using namespace valhalla::baldr;

namespace valhalla {
namespace mjolnir {

// Constructor with parameters
DirectedEdgeBuilder::DirectedEdgeBuilder(
                   const OSMWay& way, const GraphId& endnode,
                   const bool forward, const uint32_t length,
                   const uint32_t speed, const uint32_t truck_speed,
                   const baldr::Use use, const RoadClass rc,
                   const uint32_t localidx, const bool signal,
                   const uint32_t restrictions, const uint32_t bike_network)
     :  DirectedEdge() {
  set_endnode(endnode);
  set_length(length);
  set_use(use);
  set_speed(speed);    // KPH
  set_truck_speed(truck_speed); // KPH

  // Override use for ferries/rail ferries. TODO - set this in lua
  if (way.ferry()) {
    set_use(Use::kFerry);
  }
  if (way.rail()) {
    set_use(Use::kRailFerry);
  }
  set_toll(way.toll());
  set_dest_only(way.destination_only());

  if (bike_network)
    set_bike_network(way.bike_network() | bike_network);
  else
    set_bike_network(way.bike_network());

  set_truck_route(way.truck_route());

  if (!way.destination_only())
    set_dest_only(way.no_thru_traffic());

  set_surface(way.surface());
  set_cyclelane(way.cyclelane());
  set_tunnel(way.tunnel());
  set_roundabout(way.roundabout());
  set_bridge(way.bridge());
  set_link(way.link());
  set_classification(rc);
  set_localedgeidx(localidx);
  set_restrictions(restrictions);
  set_traffic_signal(signal);

  set_speed_type(way.tagged_speed() ?
        SpeedType::kTagged : SpeedType::kClassified);

  // Set forward flag and access modes (based on direction)
  set_forward(forward);
  uint32_t forward_access = 0;
  uint32_t reverse_access = 0;
  if ((way.auto_forward()  &&  forward) ||
      (way.auto_backward() && !forward)) {
    forward_access |= kAutoAccess;
  }
  if ((way.auto_forward()  && !forward) ||
      (way.auto_backward() &&  forward)) {
    reverse_access |= kAutoAccess;
  }
  if ((way.truck_forward()  &&  forward) ||
      (way.truck_backward() && !forward)) {
    forward_access |= kTruckAccess;
  }
  if ((way.truck_forward()  && !forward) ||
      (way.truck_backward() &&  forward)) {
    reverse_access |= kTruckAccess;
  }
  if ((way.bus_forward()  &&  forward) ||
      (way.bus_backward() && !forward)) {
    forward_access |= kBusAccess;
  }
  if ((way.bus_forward()  && !forward) ||
      (way.bus_backward() &&  forward)) {
    reverse_access |= kBusAccess;
  }
  if ((way.bike_forward()  &&  forward) ||
      (way.bike_backward() && !forward)) {
    forward_access |= kBicycleAccess;
  }
  if ((way.bike_forward()  && !forward) ||
      (way.bike_backward() &&  forward)) {
    reverse_access |= kBicycleAccess;
  }
  if ((way.emergency_forward()  &&  forward) ||
      (way.emergency_backward() && !forward)) {
    forward_access |= kEmergencyAccess;
  }
  if ((way.emergency_forward()  && !forward) ||
      (way.emergency_backward() &&  forward)) {
    reverse_access |= kEmergencyAccess;
  }
  if (way.pedestrian()) {
    forward_access |= kPedestrianAccess;
    reverse_access |= kPedestrianAccess;
  }

  // Set access modes
  set_forwardaccess(forward_access);
  set_reverseaccess(reverse_access);

  // TODO: HOV, Taxi?
}

}
}
