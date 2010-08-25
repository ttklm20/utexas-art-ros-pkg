/*
 *  Description:  Navigator course planning class
 *
 *  Copyright Austin Robot Technology                    
 *  All Rights Reserved. Licensed Software.
 *
 *  This is unpublished proprietary source code of Austin Robot
 *  Technology, Inc.  The copyright notice above does not evidence any
 *  actual or intended publication of such source code.
 *
 *  PROPRIETARY INFORMATION, PROPERTY OF AUSTIN ROBOT TECHNOLOGY
 *
 *  $Id$
 */

#include <art/DARPA_rules.hh>
#include <art_map/ArtLanes.h>

#include "navigator_internal.h"
#include "course.h"
#include <art_servo/steering.h>
#include <art_map/rotate_translate_transform.h>
#include <art_map/coordinates.h>
#include <art_nav/estimate.h>
using namespace Coordinates;

// Constructor
Course::Course(Navigator *_nav, int _verbose)
{
  verbose = _verbose;
  nav = _nav;

  // copy convenience pointers to Navigator class data
  estimate = &nav->estimate;
  navdata = &nav->navdata;
  odom = nav->odometry;
  order = &nav->order;
  pops = nav->pops;

  // initialize polygon vectors
  plan.clear();
  polygons.clear();

  for (unsigned i = 0; i < 2; ++i)
    adj_polys[i].clear();
  passing_lane = -1;
  passed_lane.clear();

  last_error=0;

  reset();
}

// Course class initialization for run state cycle.
//
// exit:
//	navdata->cur_poly updated
//	order->waypt array reflects last_waypt
//
void Course::begin_run_cycle(void)
{
  waypoint_checked = false;

  // Finding the current polygon is easy in a travel lane, but
  // more difficult in intersections.  They have many overlapping
  // transition lanes, and getContainingPoly() picks the first one
  // in the polygons vector, not necessarily the correct one.

  // So, first check whether vehicle is in the planned travel lane
  if (0 <= (poly_index = pops->getContainingPoly(plan, estimate->pos)))
    {
      // This is the normal case.  Re-resolve poly_index relative to
      // polygons vector.
      poly_index = pops->getPolyIndex(polygons, plan.at(poly_index));
    }
  else
    {
      // Not in the planned travel lane, check the whole road network.
      poly_index = pops->getContainingPoly(polygons, estimate->pos);
    }

  // set cur_poly ID in navdata for Commander (no longer used)
  if (poly_index < 0)			// no polygon found?
    navdata->cur_poly = -1;		// outside the road network
  else
    navdata->cur_poly = polygons.at(poly_index).poly_id;

  // This order may have been issued before Commander saw the
  // last_waypt Navigator returned in a previous cycle.  Make sure the
  // order reflects the current situation.
  int limit = N_ORDER_WAYPTS;		// search limit
  while (order->waypt[0].id != navdata->last_waypt && --limit > 0)
    {
      if (verbose >= 5)
	ART_MSG(8, "waypoint %s already reached, advance order->waypt[] array",
		order->waypt[1].id.name().str);
      // advance order->waypt array by one
      for (unsigned i = 1; i < N_ORDER_WAYPTS; ++i)
	order->waypt[i-1] = order->waypt[i];
    }

  // log current order attributes
  if (verbose >= 3)
    for (unsigned i = 0; i < N_ORDER_WAYPTS; ++i)
      ART_MSG(8, "waypt[%u] %s (%.3f,%.3f), E%d G%d L%d P%d S%d X%d Z%d",
	      i, order->waypt[i].id.name().str,
	      order->waypt[i].map.x, order->waypt[i].map.y,
	      order->waypt[i].is_entry,
	      order->waypt[i].is_goal,
	      order->waypt[i].is_lane_change,
	      order->waypt[i].is_spot,
	      order->waypt[i].is_stop,
	      order->waypt[i].is_exit,
	      order->waypt[i].is_perimeter);
}

void Course::configure(ConfigFile* cf, int section)
{
  // how far away (in seconds) we aim when changing lanes
  lane_change_secs = cf->ReadFloat(section, "lane_change_secs", 2.0);
  ART_MSG(2, "\tlane change target is %.3f seconds ahead",
	  lane_change_secs);

  // Look-ahead time for steering towards a polygon.
  lane_steer_time = cf->ReadFloat(section, "lane_steer_time", 2.0);
  ART_MSG(2, "\tlane steering time is %.3f seconds", lane_steer_time);

  heading_change_ratio = cf->ReadFloat(section, "heading_change_ratio", 0.75);
  ART_MSG(2, "\theading change ratio is %.3f", heading_change_ratio);

  turning_latency = cf->ReadFloat(section, "turning_latency", 1.0);
  ART_MSG(2, "\tturning latency time is %.3f seconds", 1.0);

  // Look-ahead time for steering towards a polygon.
  k_error = cf->ReadFloat(section, "turning_offset_tune", 0.1);
  ART_MSG(2, "\tyaw tuning parameter (offset) is %.3f", k_error);

  // Look-ahead time for steering towards a polygon.
  k_theta = cf->ReadFloat(section, "turning_heading_tune", sqrtf(k_error/2));
  ART_MSG(2, "\tyaw tuning parameter (heading) is %.3f", k_theta);

  // Look-ahead time for steering towards a polygon.
  yaw_ratio = cf->ReadFloat(section, "yaw_ratio", 0.75);
  ART_MSG(2, "\tyaw ratio is %.3f", yaw_ratio);

  // Look-ahead time for steering towards a polygon.
  k_int = cf->ReadFloat(section, "turning_int_tune", 1.5);
  ART_MSG(2, "\tyaw tuning parameter (integral) is %.3f", k_int);


  // Minimum distance to aim for when changing lanes.
  // Should at least include front bumper offset and minimum separation.
  min_lane_change_dist = cf->ReadFloat(section, "min_lane_change_dist",
				       (DARPA_rules::min_forw_sep_travel
					+ ArtVehicle::front_bumper_px));
				       
  ART_MSG(2, "\tminimum lane change distance is %.3f meters",
	  min_lane_change_dist);

  // Minimum look-ahead distance for steering towards a polygon.
  // Should at least include front bumper offset.
  min_lane_steer_dist = cf->ReadFloat(section, "min_lane_steer_dist",
				      ArtVehicle::front_bumper_px);
  ART_MSG(2, "\tminimum lane steering distance is %.3f meters",
	  min_lane_steer_dist);

  // plan way-point limit.  Only for testing Navigator's ability to
  // run with a truncated course plan.  Do not set otherwise.
  plan_waypt_limit = cf->ReadInt(section, "plan_waypt_limit", N_ORDER_WAYPTS);
  if (plan_waypt_limit < 2 || plan_waypt_limit > N_ORDER_WAYPTS)
    plan_waypt_limit = N_ORDER_WAYPTS;
  ART_MSG(2, "\tplan_waypt limit is %d", plan_waypt_limit);

  //How fast the maximum steer can be done
  max_speed_for_sharp=cf->ReadFloat(section, "max_speed_for_sharp",3.0); 
  ART_MSG(2, "\tmaximum speed to go full yaw is %.3f m", max_speed_for_sharp);

  // desired passing distance
  spring_lookahead = cf->ReadFloat(section, "spring_lookahead", 0.0);
  ART_MSG(2, "\tspring lookahead distance is %.3f m", spring_lookahead);


  max_yaw_rate = 
    cf->ReadFloat(section, "real_max_yaw_rate", Steering::maximum_yaw);
  ART_MSG(2, "\treal_max_rate_rate is %.3f m", max_yaw_rate);

  zone_waypoint_radius = cf->ReadFloat(section, "zone_waypoint_radius", 1.0);
  ART_MSG(2, "\tzone waypoint radius is %.3f m", zone_waypoint_radius);

  zone_perimeter_radius = cf->ReadFloat(section, "zone_perimeter_radius", 2.0);
  ART_MSG(2, "\tzone perimeter radius is %.3f m", zone_perimeter_radius);

  spot_waypoint_radius = cf->ReadFloat(section, "spot_waypoint_radius", 0.5);
  ART_MSG(2, "\tzone waypoint radius is %.3f m", spot_waypoint_radius);
}

// set heading for desired course
//
// entry:
//	plan contains desired polygon path to follow
//      offset_ratio 1.0 pushes the left side of the car to the left
//		lane boundary, 0.0 to the center, -1.0 to the right.
//		Larger offsets push the car outside the lane.
//
void Course::desired_heading(pilot_command_t &pcmd, float offset_ratio)
{

  if (Epsilon::equal(pcmd.velocity,0.0))
    return;

  Polar aim_polar;			// egocentric polar aim point
  //  float aim_abs_heading=0;
  float aim_next_heading=0;
  float aim_distance=0;
  bool aim_in_plan=false;
  int aim_index=-1;


  
#if 0
  float used_velocity=fmaxf(estimate->vel.px,pcmd.velocity);
  float target_dist=min_lane_steer_dist+lane_steer_time*estimate->vel.px;//used_velocity;
#else
  float used_velocity=estimate->vel.px;
  float target_dist=min_lane_steer_dist;
#endif
  
  if (plan.empty())
    {
      // no plan available: a big problem, but must do something
      if (verbose >= 2)
	ART_MSG(5, "no lane data available, steer using waypoints.");
      aim_polar = head_for_waypt(target_dist);
      aim_distance=aim_polar.range;
      aim_next_heading=Coordinates::normalize(estimate->pos.pa+aim_polar.heading);
    }
  else 
    {
      // Look in plan
      aim_index = pops->getPolyIndex(plan, aim_poly);
      
      poly_list_t edge;
      pops->add_polys_for_waypts(plan,edge,order->waypt[0].id,
				 order->waypt[1].id);
      
      // get closest polygon to estimated position
      int nearby_poly = pops->getClosestPoly(edge, estimate->pos);
      if (nearby_poly >= 0)
	nearby_poly=pops->getPolyIndex(plan,edge.at(nearby_poly));
      else  nearby_poly=pops->getClosestPoly(plan, estimate->pos);

      if (aim_poly.poly_id != -1 && aim_index >=0 && 
	  aim_index < (int)plan.size()-1)
	{
	  if (nearby_poly >= 0)
	    {
	      int aim_index2=pops->index_of_downstream_poly
		(plan,nearby_poly,target_dist);	      
	  
	      if (aim_index2 > aim_index && aim_index2 < (int) plan.size()-1) 
		{
		  aim_index=aim_index2;
		  aim_poly.poly_id = -1; // no aim polygon defined
		}
	    }
	  // If find_aim_polygon was recently called by
	  // find_travel_lane or by switch_to_passing_lane use the
	  // aim_poly
	  
	  //	  aim_polar = MapXY_to_Polar(pops->getPolyEdgeMidpoint
	  //				     (plan.at(aim_index)), estimate->pos);
	  
	  //	  aim_abs_heading=Coordinates::normalize
	  //	    (estimate->pos.pa+aim_polar.heading);
	  aim_distance = Euclidean::DistanceTo(plan.at(aim_index+1).midpoint,
					       plan.at(aim_index).midpoint);
	  aim_next_heading = atan2f(plan.at(aim_index+1).midpoint.y-
				    plan.at(aim_index).midpoint.y,
				    plan.at(aim_index+1).midpoint.x-
				    plan.at(aim_index).midpoint.x);

	  aim_in_plan=true;
	  
	  if (verbose >= 3)
	    ART_MSG(8, "steering down the lane toward polygon %d",
		    plan.at(aim_index).poly_id);
	}
      else
	{
	  if (nearby_poly >= 0)
	    {
	      if (verbose >= 4)
		ART_MSG(8, "nearby_poly in desired_heading() is %d",
			plan.at(nearby_poly).poly_id);
	      
	      // set aim_polar to the closest polygon at least target_dist
	      // meters away from the estimated position.
	      
	      aim_index=pops->index_of_downstream_poly
		(plan,nearby_poly,target_dist);
	      if (aim_index >=0 && aim_index < (int)plan.size()-1)
		{
		  // Polygon at target distance
		  //		  aim_polar = MapXY_to_Polar
		  //		    (plan.at(aim_index).midpoint,
		  //		     estimate->pos);
		  
		  //		  aim_abs_heading=Coordinates::normalize
		  //		    (estimate->pos.pa+aim_polar.heading);
		  
		  aim_distance = Euclidean::DistanceTo(plan.at(aim_index+1).midpoint,
						       plan.at(aim_index).midpoint);

		  aim_next_heading = atan2f(plan.at(aim_index+1).midpoint.y-
					    plan.at(aim_index).midpoint.y,
					    plan.at(aim_index+1).midpoint.x-
					    plan.at(aim_index).midpoint.x);

		  aim_in_plan=true;
		  
		  if (verbose >= 3)
		    ART_MSG(8, "steering at least %.3fm down the lane toward polygon %d",
			    target_dist, plan.at(aim_index).poly_id);
		}
	      else
		// No polygon in target distance.  Head to next
		// waypoint
		{
		  ART_MSG(8, "no polygon at least %.3fm away, steer using waypoints", target_dist);
		  aim_polar = head_for_waypt(target_dist);
		  
		  aim_distance=aim_polar.range;
		  aim_next_heading=Coordinates::normalize
		    (estimate->pos.pa+aim_polar.heading);
		}
	    }
	  else
	    {
	      // no plan available: a big problem, but must do
	      // something.  Go to next waypoint.
	      
	      if (verbose >= 2)
		ART_MSG(5, "no lane data available, steer using waypoints.");
	      aim_polar = head_for_waypt(target_dist);
	      aim_distance=aim_polar.range;
	      aim_next_heading=Coordinates::normalize
		(estimate->pos.pa+aim_polar.heading);
	    }
	}
    }
  
  
  if (verbose >= 3)
    {
      ART_MSG(8, "desired, current positions: (%.3f, %.3f), (%.3f, %.3f, %.3f)",
	      order->waypt[1].map.x, order->waypt[1].map.y,
	      estimate->pos.px, estimate->pos.py, estimate->pos.pa);
      //      ART_MSG(8, "desired relative heading: %.3f radians, "
      //	      "distance: %.3f meters",
      //	      aim_polar.heading, aim_polar.range);
    }
  
  float full_heading_change=fabs(Coordinates::normalize(aim_next_heading-estimate->pos.pa));
  //fabsf(aim_polar.heading);//+
  //    fabsf(Coordinates::normalize(aim_next_heading-aim_abs_heading));
  
  float max_speed_to_hit_aim=
    max_speed_for_change_in_heading(full_heading_change,
				    aim_distance,
				    pcmd.velocity,
				    max_yaw_rate);

  pcmd.velocity = fminf(pcmd.velocity,
			max_speed_to_hit_aim);
  


#if 0
  if (pcmd.velocity>used_velocity)
    used_velocity+=fminf(pcmd.velocity-used_velocity,1.0*turning_latency);
  else if (pcmd.velocity < used_velocity)
    used_velocity-=fminf(used_velocity-pcmd.velocity,1.0*turning_latency);
#endif

#if 1
  used_velocity=fmaxf(pcmd.velocity,used_velocity);
#endif
  
#if 0
  used_velocity=fminf(used_velocity,pcmd.velocity);
#endif

  if (verbose >= 3)
    ART_MSG(8,"Thresholding speed to %.3f m/s",used_velocity);

  float spring_yaw;
  if (aim_in_plan)
    spring_yaw=get_yaw_spring_system(aim_polar, aim_index, 
				     aim_next_heading,
				     max_yaw_rate, used_velocity,
				     offset_ratio);
  else
    spring_yaw=get_yaw_spring_system(aim_polar, -1, aim_next_heading,
				     max_yaw_rate, used_velocity);
  
  pcmd.yawRate = spring_yaw;

#if 0
  if (Epsilon::equal(pcmd.yawRate,max_yaw_rate))
    pcmd.velocity=fminf(pcmd.velocity,Steering::steer_speed_min);
#endif  

  nav->trace_controller("desired_heading", pcmd);
}

// return distance in the plan to a way-point
float Course::distance_in_plan(const player_pose2d_t &from,
			       const WayPointNode &wp) const
{
  if (plan.empty())
    return Euclidean::DistanceToWaypt(from, wp);
  else return pops->distanceAlongLane(plan, MapXY(from), wp.map);
}

// return distance in plan to a pose
float Course::distance_in_plan(const player_pose2d_t &from,
			       const player_pose2d_t &to) const
{
  if (plan.empty())
    return Euclidean::DistanceTo(from, to);
  else return pops->distanceAlongLane(plan, MapXY(from), MapXY(to));
}

float Course::distance_in_plan(const player_pose2d_t &from,
			       const MapXY &to) const
{
  if (plan.empty())
    return Euclidean::DistanceTo(from, to);
  else return pops->distanceAlongLane(plan, MapXY(from),to);
}


// Course class termination for run state cycle.
//
// entry:
//	waypoint_checked true if any controller has checked that a new
//	way-point has been reached.
//
void Course::end_run_cycle()
{
  if (!waypoint_checked)
    ART_MSG(1, "failed to check for way-point reached!");
}

// find an aim polygon ahead of the car in lane
//
//  The selected polygon is at least min_lane_change_dist away.  The
//  exact choice depends on the distance of the car from the lane, and
//  its velocity.
//
// returns: index of aim polygon, -1 if none
//
int Course::find_aim_polygon(poly_list_t &lane)
{

  poly_list_t edge;
  pops->add_polys_for_waypts(lane,edge,order->waypt[0].id,
			     order->waypt[1].id);
  
  // get closest polygon to estimated position
  int nearby_poly = pops->getClosestPoly(edge, estimate->pos);
  if (nearby_poly < 0)
    nearby_poly = pops->getClosestPoly(lane, estimate->pos);
  else
    nearby_poly=pops->getPolyIndex(lane,edge.at(nearby_poly));

  if (nearby_poly < 0)
    return -1;
  
#if 1  
  float aim_distance = min_lane_steer_dist;
  
  if (verbose >= 4)
    ART_MSG(8, "aim point %.3fm ahead", aim_distance);
  
  return pops->index_of_downstream_poly(lane, nearby_poly, aim_distance);

#else

  // increase aim_distance if the lane is far away
  float lane_distance =
    pops->getShortestDistToPoly(estimate->pos, lane.at(nearby_poly));
  
  if (Epsilon::equal(lane_distance,0.0))
    return -1;
   
  float lane_change_distance_ratio = estimate->vel.px;
  float aim_distance;

  if (req_max_dist < Infinite::distance)
    aim_distance=req_max_dist;
  else 
    {  
      // Try based on speed -- may be too far or 0 (if not moving)
      aim_distance = fminf(lane_distance * lane_change_distance_ratio,
			   estimate->vel.px * lane_change_secs);
    }

  float max_pass_distance=ArtVehicle::length*4;

  // Threshold by maximum distance to get over.
  aim_distance = fminf(max_pass_distance,aim_distance);

  if (order->waypt[1].is_goal)
    {
      float way_dist=distance_in_plan(estimate->pos, order->waypt[1]);
      aim_distance=fminf(aim_distance,way_dist);
    }

  // At least look as far ahead as bumper
  aim_distance = fmaxf(aim_distance, ArtVehicle::front_bumper_px);

  if (verbose >= 4)
    ART_MSG(8, "lane is %.3fm away, aim point %.3fm ahead",
	    lane_distance, aim_distance);
  
  return pops->index_of_downstream_poly(lane, nearby_poly, aim_distance);
  
#endif
}

// Find an appropriate polygon path for passing an obstacle blocking
// the current travel lane.
//
// Note that an obstacle could be on a checkpoint.  DARPA said that
// checkpoints would be in driveable locations.  But, another car
// could still stop there.  Our implementation will pass it anyway and
// consider the checkpoint reached when the car passes it.  That does
// not meet the requirement that the front bumper pass over the
// checkpoint, but follows the DARPA Technical FAQ recommendation.
//
// exit:
//	sets adj_lane[passing_lane], adj_polys[passing_lane]
//	sets passing_left true iff passing to the left
//	leaves Course::plan alone
//	returns true, if alternate lane found
//
bool Course::find_passing_lane(void)
{
  if (verbose)
    ART_MSG(5, "find passing lane around waypoint %s",
	    order->waypt[1].id.name().str);

  // generate adjacent lane IDs
  adj_lane[0] = order->waypt[1].id;
  --adj_lane[0].lane;			// next lower lane number

  adj_lane[1] = order->waypt[1].id;
  ++adj_lane[1].lane;			// next higher lane number

#if 1 // more general implementation, experimental

  int cur_index = pops->getClosestPoly(plan, estimate->pos);
  if (cur_index == -1)
    {
      if (verbose)
	ART_MSG(1, "no polygon nearby in plan");
      return false;
    }
  poly cur_poly = plan.at(cur_index);
  
  // collect polygons for any adjacent lanes and determine their
  // relative position and direction.
  int left_lane = -1;			// no left lane
  int right_lane = -1;			// no right lane
  bool adj_forw[2];			// true if going forward

  for (unsigned i = 0; i < 2; ++i)
    {
      adj_lane[i].pt = 0;		// lane ID, not way-point
      adj_polys[i].clear();
      if (adj_lane[i].lane == 0)	// ID not valid?
	continue;

      // collect lane polygons
      pops->AddLanePolys(polygons, adj_polys[i], adj_lane[i]);
      int this_index = pops->getClosestPoly(adj_polys[i],
					    order->waypt[1].map);
      if (this_index < 0)		// no polygon found?
	continue;

      // see if it is right or left of current lane
      poly this_poly = adj_polys[i].at(this_index);
      if (pops->left_of_poly(this_poly, cur_poly))
	left_lane = i;
      else
	right_lane = i;

      // see if it goes forward or backward
      adj_forw[i] = pops->same_direction(cur_poly, this_poly, HALFPI);
      if (!adj_forw[i])
	{
	  // collect polygons in reverse direction, instead
	  adj_polys[i].clear();
	  pops->AddReverseLanePolys(polygons, adj_polys[i], adj_lane[i]);
	}

      if (verbose >= 4)
	log(adj_lane[i].lane_name().str, adj_polys[i]);
    }

  // pick the preferred lane and direction
  if (right_lane >= 0
      && adj_forw[right_lane])
    passing_lane = right_lane;		// right lane, forward
  else if (left_lane >= 0
      && adj_forw[left_lane])
    passing_lane = left_lane;		// left lane, forward
  else if (right_lane >= 0)
    passing_lane = right_lane;		// right lane, backward
  else if (left_lane >= 0)
    passing_lane = left_lane;		// left lane, backward
  else
    {
      passing_lane = -1;
      if (verbose)
	ART_MSG(1, "no passing lane available for waypoint %s",
		order->waypt[1].id.name().str);
      return false;
    }

  // save direction for turn signals
  passing_left = (passing_lane == left_lane);

  if (verbose)
    ART_MSG(5, "passing lane %s selected, to %s going %s",
	    adj_lane[passing_lane].lane_name().str,
	    (passing_left? "left": "right"),
	    (adj_forw[passing_lane]? "forward": "backward"));

#else // old version

  for (unsigned i = 0; i < 2; ++i)
    {
      // TODO: use pops->PolyHeading() to determine direction of
      // passing lane.  (This code reproduces site visit capability,
      // so it is always reversed.)
      adj_lane[i].pt = 0;		// lane ID, not way-point
      adj_polys[i].clear();
      if (adj_lane[i].lane > 0)	// ID in valid range?
	{
	  pops->AddReverseLanePolys(polygons, adj_polys[i], adj_lane[i]);
	}
      if (verbose >= 4)
	log(adj_lane[i].lane_name().str, adj_polys[i]);
    }

  // find a non-empty lane
  passing_lane = 1;
  while (passing_lane >= 0 && adj_polys[passing_lane].empty())
    {
      --passing_lane;
    }

  // if there are no polygons at all, or no adjacent lane in this
  // segment, return failure
  if (passing_lane < 0)
    {
      if (verbose)
	ART_MSG(1, "no passing lane available for waypoint %s",
		order->waypt[1].id.name().str);
      return false;
    }

  passing_left = true;			// always left for now

  if (verbose)
    ART_MSG(5, "passing lane %s selected",
	    adj_lane[passing_lane].lane_name().str);

#endif

  return true;
}

// Find a path in the travel lane to the next few way-points.
//
// We want the sequence of polygons that will take us from our current
// position to the order->waypt[].id polygons, avoiding wrong paths
// through any intersection.
//
// entry: rejoin is true when the car is currently outside the lane
//
void Course::find_travel_lane(bool rejoin)
{
  if (plan_valid())
    {
      if (verbose >= 4)
	ART_MSG(5, "find_travel_lane() plan still valid");
    }
  else
    {
      // make a new plan
      plan.clear();
      aim_poly.poly_id = -1;		// no aim polygon defined
      set_plan_waypts();
    
      if (polygons.size() == 0)		// no lane data available?
	{
	  if (verbose >= 2)
	    ART_MSG(5, "find_travel_lane() has no polygons");
	  return;
	}

      // push waypt[0] polygon onto the plan
      pops->add_polys_for_waypts(polygons, plan,
				 order->waypt[0].id, order->waypt[0].id);
      if (verbose >= 6) log("debug plan", plan);

      // add polygons leading to the target waypt entries
      for (int i = 1; i < plan_waypt_limit; ++i)
	{
	  // Do not repeat polygons for repeated way-points in the order.
	  if (order->waypt[i-1].id != order->waypt[i].id)
	    // Collect all polygons from previous waypt to this one and
	    // also the polygon containing this one.
	    pops->add_polys_for_waypts(polygons, plan,
				       order->waypt[i-1].id,
				       order->waypt[i].id);
	  // don't plan past a zone entry
	  if (order->waypt[i].is_perimeter)
	    break;
	}

      if (plan.size() > 1 && verbose >= 6)
	{
	  ART_MSG(7, "plan[0] start, end waypoints are %s, %s, poly_id = %d",
		  plan.at(0).start_way.name().str,
		  plan.at(0).end_way.name().str,
		  plan.at(0).poly_id);
	  ART_MSG(7, "plan[1] start, end waypoints are %s, %s, poly_id = %d",
		  plan.at(1).start_way.name().str,
		  plan.at(1).end_way.name().str,
		  plan.at(1).poly_id);
	}
      log("find_travel_lane() plan", plan);
    }
  
  new_plan_lanes = false;		// plan reflects current lanes
  aim_poly.poly_id = -1;		// no aim polygon defined

  if (rejoin)
    {
      // If the car is outside its lane, select appropriate polygon to
      // rejoin it.  Otherwise, the car may overshoot and circle back,
      // which would be very bad.  This also prevents the follow
      // safely controller from getting confused after passing an
      // obstacle in the target lane.

      // Beware: when approaching the site visit intersection the loop
      // in segment one causes find_aim_polygon(lane_polys) to pick
      // the wrong end of the lane, causing the car to turn the wrong
      // way.  So, first look it up in plan, then find the
      // corresponding index in lane_polys.

      // find a polygon slightly ahead of the car
      int aim_index = find_aim_polygon(plan);
      if (aim_index >= 0)
	{
	  // set aim polygon for obstacle avoidance
	  aim_poly = plan.at(aim_index);
	  if (verbose >= 2)
	    ART_MSG(5, "aim polygon is %d", aim_poly.poly_id);
	}
    }
}

// Head directly for next reachable way-point.
//
// This is trouble: the plan stops too soon for navigating
// by polygons.  Have to do something, so head directly for
// the next way-point, but make sure it's far enough away
// that the car does not double back to it.
Polar Course::head_for_waypt(float target_dist)
{
  Polar aim_polar = MapXY_to_Polar(order->waypt[1].map, estimate->pos);
  if (aim_polar.range < target_dist)
    {
      if (special_waypt(1))
	{
	  // If the next way-point is a stop or U-turn, go straight
	  // and try to reach it.
	  ART_MSG(8, "waypt[1] is a special way-point, keep current heading");
	  aim_polar.heading=0.0;
	}
      else if (order->waypt[1].is_perimeter)
	{
	  ART_MSG(8, "waypt[1] is a perimeter point");
	  aim_polar = MapXY_to_Polar(order->waypt[1].map, estimate->pos);
	  if (fabsf(Coordinates::bearing(estimate->pos,order->waypt[1].map)) >
	      HALFPI)
	    new_waypoint_reached(order->waypt[1].id);
	}
      else
	{
	  // waypt[1] is too close, steer for waypt[2] instead
	  aim_polar = MapXY_to_Polar(order->waypt[2].map, estimate->pos);
	  ART_MSG(8, "waypt[1] less than %.3fm away, using waypt[2] instead",
		  target_dist);
	  // claim we got there (we're at least close)
	  new_waypoint_reached(order->waypt[1].id);
	}
    }
  return aim_polar;
}

// return lane change direction
Course::direction_t Course::lane_change_direction(void)
{
  int w0_index = pops->get_waypoint_index(polygons, order->waypt[0].id);
  int w1_index = pops->get_waypoint_index(polygons, order->waypt[1].id);

  // give up unless both polygons are available
  if (w0_index < 0 || w1_index < 0)
    return Straight;

  if (pops->left_of_poly(polygons.at(w1_index), polygons.at(w0_index)))
    return Left;
  else
    return Right;
}

// check if lane way-point reached
//
// Considers a way-point reached when the car is in front of the pose
// formed by the way-point and the heading of its containing polygon.
//
// exit: navdata->last_waypt updated
// returns: true if order->waypt[1] reached (unless a special way-point)
//
// bugs: Does not work for zone perimeter way-points because they do
//	 not have a containing polygon.  Those are detected by the
//	 stop_line controller, instead.
//
bool Course::lane_waypoint_reached(void)
{
  // Mark the way-point checked, even if it is a special one.
  waypoint_checked = true;

  if (order->waypt[1].is_perimeter)
    return zone_perimeter_reached();
  
  // Special way-points (stop, U-turn) are handled explicitly
  // elsewhere by their state-specific controllers.  They cause state
  // transitions, so they must be ignored here and only considered
  // "reached" when the requirements of those specific controllers are
  // fully met.
  if (special_waypt(1))
    return false;

#ifdef USE_PATS
  ElementID last_way = pops->updateLaneLocation(polygons,
						odom->pose,
						order->waypt[0],
						order->waypt[1]);
  if (last_way == order->waypt[1].id)
    {
      navdata->last_way = last_way;
      return true;
    }
  return false;
#endif

  bool found = false;

  // Instead of checking a circle about the way-point, see if the
  // car has reached a line through the way-point perpendicular to
  // the direction of its lane.

  // get polygon index of waypt[1] (TODO: save somewhere)
  int w1_index = -1;

  // TURNING ON SHOULD BE OK.  KEEP OFF UNTIL WE ARE CERTAIN.
#if 1
  w1_index = pops->get_waypoint_index(polygons, order->waypt[1].id);
#else
  w1_index = pops->getContainingPoly(polygons, order->waypt[1].map);
#endif
  
  if (w1_index >= 0)
    {
      // form way-point pose using polygon heading
      // TODO: save somewhere
      MapPose w1_pose(order->waypt[1].map, 
		      pops->PolyHeading(polygons.at(w1_index)));
      
#if 1
      // Is the bearing of the car from that pose within 90
      // degrees of the polygon heading?
      float bearing_from_w1 = bearing(w1_pose, MapXY(odom->curr_pos.pos));
#else // experimental code -- not working right yet
      // Is the bearing of a point slightly ahead of the front bumper
      // from that pose within 90 degrees of the polygon heading?
      Polar bumper_polar(0.0,
			 (ArtVehicle::front_bumper_px
			  + DARPA_rules::stop_line_to_bumper));
      MapXY bumper_pos = Polar_to_MapXY(bumper_polar, odom->pos);
      float bearing_from_w1 = bearing(w1_pose, bumper_pos);
#endif
      if (fabsf(bearing_from_w1) < DTOR(90))
	{
	  // The car is "in front" of this way-point's pose.
	  if (verbose)
	    ART_MSG(2, "reached waypoint %s, bearing %.3f radians",
		    order->waypt[1].id.name().str, bearing_from_w1);
	  navdata->last_waypt = order->waypt[1].id;
	  found = true;
	}
    }

  
  if (!found && verbose >= 5)
    ART_MSG(8, "cur_poly = %d, last_waypt = %s",
	    navdata->cur_poly, navdata->last_waypt.name().str);
  return found;
}

// handle lanes message
//
//  Called from the driver ProcessMessage() handler when new lanes
//  data arrive.
//
void Course::lanes_message(lanes_state_msg_t* lanes)
{
  polygons.resize(lanes->poly_count);

  for (unsigned num = 0; num < lanes->poly_count; num++)
    polygons.at(num) = lanes->poly[num];

  if (polygons.empty())
    ART_MSG(1, "empty lanes polygon list received!");

  // force plan to be recomputed
  new_plan_lanes = true;

  log("lanes input:", polygons);
};

// log a vector of polygons
void Course::log(const char *str, const poly_list_t &polys)
{
  unsigned npolys = polys.size();
  if (npolys > 0)
    {
      if (verbose >= 3)
	{
	  for (unsigned i = 0; i < npolys; ++i)
	    {
#if 0
	      if (verbose >= 5)
		ART_MSG(8, "polygon[%u] = %d", i, polys.at(i).poly_id);
#endif
	      unsigned start_seq = i;
	      while (i+1 < npolys
		     && abs(polys.at(i+1).poly_id - polys.at(i).poly_id) == 1)
		{
#if 0
		  if (verbose >= 5)
		    ART_MSG(8, "polygon run from %u (%d) to %u (%d)",
			    i, polys.at(i).poly_id,
			    i+1, polys.at(i+1).poly_id);
#endif
		  ++i;
		}
	      if (start_seq == i)
		ART_MSG(8, "%s polygon at %d", str, polys.at(i).poly_id);
	      else
		ART_MSG(8, "%s polygons from %d to %d",
			str, polys.at(start_seq).poly_id, polys.at(i).poly_id);
	    }
	}
    }
  else
    {
      if (verbose >= 2)
	ART_MSG(8, "%s no polygons at all", str);
    }
}

// return true if current order does not match saved way-points
bool Course::new_waypts(void)
{
  if (saved_replan_num!=order->replan_num)
    return true;

  for (unsigned i = 0; i < N_ORDER_WAYPTS; ++i)
    if (saved_waypt_id[i] != order->waypt[i].id)
      return true;

  // still the same
  return false;
}

// reset course class
void Course::reset(void)
{
  if (verbose)
    ART_MSG(2, "Course class reset()");

  // TODO: figure out when this needs to happen and what to do
  start_pass_location.px = 0.0;
  start_pass_location.py = 0.0;
  start_pass_location.pa = 0.0;

  // clear the previous plan
  plan.clear();
  aim_poly.poly_id = -1;
}

// replan after road block
ElementID Course::replan_roadblock(void)
{
  saved_replan_num=order->replan_num;

  // save current order way-points
  for (unsigned i = 0; i < N_ORDER_WAYPTS; ++i)
    {
      saved_waypt_id[i] = order->waypt[i].id;
      if (verbose >= 4)
	ART_MSG(8, "saved_waypt_id[%u] = %s",
		i, saved_waypt_id[i].name().str);
    }

  // Get closest polygon in current plan.
  int uturn_exit_index = 
    pops->getClosestPoly(plan,estimate->pos);

  player_pose2d_t exit_pose;
  exit_pose.px=plan.at(uturn_exit_index).midpoint.x;
  exit_pose.py=plan.at(uturn_exit_index).midpoint.y;

  // Should get lane left of current position.  If in transition, the
  // lane should be left of previous lane left.
  ElementID reverse_lane=pops->getReverseLane(polygons,exit_pose);

  if (verbose >= 4)
    ART_MSG(5,"Replan from lane %s", reverse_lane.lane_name().str);

  return reverse_lane;
}

// direction for crossing an intersection
Course::direction_t Course::intersection_direction(void)
{
  int w0_index = pops->getContainingPoly(polygons, order->waypt[0].map);
  int w1_index = pops->getContainingPoly(polygons, order->waypt[1].map);

  // give up unless both polygons are available
  if (w0_index < 0 || w1_index < 0)
    return Straight;

  float w0_heading = pops->PolyHeading(polygons.at(w0_index));
  float w1_heading = pops->PolyHeading(polygons.at(w1_index));
  float heading_change = normalize(w1_heading - w0_heading);
					    
  if (verbose >= 4)
    ART_MSG(5, "heading change from waypoint %s to %s is %.3f radians",
	    order->waypt[0].id.name().str,
	    order->waypt[1].id.name().str,
	    heading_change);

  if (fabsf(heading_change) < DTOR(30))
    return Straight;
  else if (heading_change > 0.0)
    return Left;
  else
    return Right;
}

// true if order has an upcoming stop way-point
//
// exit: sets stop_waypt, stop_poly if found
//
float Course::stop_waypt_distance(bool same_lane)
{
  for (unsigned i = 1; i < N_ORDER_WAYPTS; ++i)
    {
      // only consider way-points in the current lane
      if (same_lane
	  && !order->waypt[i].id.same_lane(order->waypt[0].id))
	break;

      if (order->waypt[i].is_stop)
	{
	  // find stop way-point polygon
	  int stop_index = pops->getContainingPoly(polygons,
						   order->waypt[i].map);
	  if (stop_index < 0)		// none found?
	    continue;			// keep looking

	  stop_poly = polygons.at(stop_index);
	  stop_waypt = order->waypt[i];
	  float wayptdist = distance_in_plan(estimate->pos, stop_waypt);
	  if (verbose >= 2)
	    ART_MSG(5, "Stop at waypoint %s is %.3fm away",
		    stop_waypt.id.name().str, wayptdist);
	  return wayptdist;
	}
    }
  return Infinite::distance;
}

// switch to previously selected passing lane
//
// entry:
//	adj_lane[passing_lane].id is the passing lane ID
//	adj_polys[passing_lane] contains its polygons
//	obstacle is polar coordinate of nearest obstacle in plan
// exit:
//	plan contains polygons to follow
//	passed_lane contains previous plan polygons
//	start_pass_location set to current pose, with polygon heading
//	returns true if successful
//
bool Course::switch_to_passing_lane()
{
  // find a polygon slightly ahead of the car
  int aim_index = find_aim_polygon(adj_polys[passing_lane]);
  if (aim_index == -1)
    {
      if (verbose)
	ART_MSG(2, "unable to pass, no polygon near the aiming point");
      return false;
    }

  // save original plan for checking when it is safe to return
  passed_lane = plan;

  // collect all the polygons from aim_index to end of passing lane
  plan.clear();
  pops->CollectPolys(adj_polys[passing_lane], plan, aim_index);
  
  log("switch_to_passing_lane() plan", plan);
  if (plan.empty())
    {
      if (verbose)
	ART_MSG(2, "no polygons in passing lane past aiming point");
      return false;
    }

  aim_poly=plan.at(0);
  MapXY aim_poly_midpt = pops->getPolyEdgeMidpoint(aim_poly);
  if (verbose >= 2)
    ART_MSG(5, "aiming at polygon %d, midpoint (%.3f, %.3f)",
	    aim_poly.poly_id, aim_poly_midpt.x, aim_poly_midpt.y);

  MapXY start_point=pops->GetClosestPointToLine
    (pops->midpoint(aim_poly.p1,aim_poly.p4),
     pops->midpoint(aim_poly.p2,aim_poly.p3),
     estimate->pos,true);

  start_pass_location.px=start_point.x;
  start_pass_location.py=start_point.y;
  start_pass_location.pa=aim_poly.heading;

  ART_MSG(1, "passing starts at (%.3f, %.3f)",
	  start_pass_location.px, start_pass_location.py);

  return true;
}

// return distance to upcoming U-turn way-point, Infinite::distance if none.
float Course::uturn_distance(void)
{
  int i = uturn_order_index();
  if (i < 0)
    return Infinite::distance;

  // find stop way-point polygon
  int stop_index = pops->getContainingPoly(polygons, order->waypt[i].map);
  if (stop_index < 0)		// none found?
    return Infinite::distance;

  // save way-point and polygon for stop_line controller
  stop_poly = polygons.at(stop_index);
  stop_waypt = order->waypt[i];

  // compute distance remaining
  float wayptdist = distance_in_plan(estimate->pos, stop_waypt);
  if (verbose >= 2)
    ART_MSG(5, "U-turn at waypoint %s, %.3fm away",
	    stop_waypt.id.name().str, wayptdist);
  return wayptdist;
}

// return index of upcoming U-turn transition in order->waypt array
//
// A U-turn is represented in the RNDF by an exit way-point in one
// lane pointing to a matching entry way-point in an adjacent lane.
//
int Course::uturn_order_index(void)
{
  for (unsigned i = 1; i < N_ORDER_WAYPTS-1; ++i)
    {
      // only consider way-points in the current lane
      if (!order->waypt[i].id.same_lane(order->waypt[0].id))
	break;
      
      if (uturn_waypt(i))
	return i;
    }
  return -1;
}

// return true if waypt[windex] and waypt[windex+1] are a U-turn pair
bool Course::uturn_waypt(unsigned windex)
{
  if (order->next_uturn < 0)
    return false;
    
  return (windex==(unsigned)order->next_uturn);
}

// check if zone way-point reached
//
// Considers a way-point reached when the front of the car is within
// zone_waypoint_radius of the way-point.
//
// exit: navdata->last_waypt updated
// returns: true if order->waypt[1] reached
//
bool Course::zone_waypoint_reached(void)
{
  bool found = false;
  waypoint_checked = true;
	  
  // polar coordinate of front bumper from estimated position
  Polar bumper_polar(0.0, ArtVehicle::front_bumper_px);
  float distance = Euclidean::DistanceToWaypt(bumper_polar, estimate->pos,
					      order->waypt[1]);

  if (distance <= zone_waypoint_radius)
    {
      // The car is near this way-point.
      if (verbose)
	ART_MSG(2, "reached zone waypoint %s, distance %.3fm",
		    order->waypt[1].id.name().str, distance);
      navdata->last_waypt = order->waypt[1].id;
      found = true;
    }
  else
    {
      if (verbose >= 5)
	ART_MSG(2, "distance to zone waypoint %s is %.3fm",
		order->waypt[1].id.name().str, distance);
    }
  
  return found;
}

bool Course::zone_perimeter_reached(void)
{
  bool found = false;
  waypoint_checked = true;
  
  int w1_index = pops->getClosestPoly(polygons, order->waypt[1].map);
  if (w1_index >= 0)
    {
      // form way-point pose using polygon heading
      // TODO: save somewhere
      MapPose w1_pose(order->waypt[1].map, 
		      pops->PolyHeading(polygons.at(w1_index)));
      
#if 1
      // Is the bearing of the car from that pose within 90
      // degrees of the polygon heading?
      float bearing_from_w1 = bearing(w1_pose, MapXY(odom->curr_pos.pos));
#else // experimental code -- not working right yet
      // Is the bearing of a point slightly ahead of the front bumper
      // from that pose within 90 degrees of the polygon heading?
      Polar bumper_polar(0.0,
			 (ArtVehicle::front_bumper_px
			  + DARPA_rules::stop_line_to_bumper));
      MapXY bumper_pos = Polar_to_MapXY(bumper_polar, odom->pos);
      float bearing_from_w1 = bearing(w1_pose, bumper_pos);
#endif
      if (fabsf(bearing_from_w1) < DTOR(90))
	{
	  // The car is "in front" of this way-point's pose.
	  if (verbose)
	    ART_MSG(2, "reached waypoint %s, bearing %.3f radians",
		    order->waypt[1].id.name().str, bearing_from_w1);
	  navdata->last_waypt = order->waypt[1].id;
	  found = true;
	}
    }
  
  return found;
}


bool Course::spot_waypoint_reached(void)
{
  bool found = false;
  waypoint_checked = true;
	  
  // polar coordinate of front bumper from estimated position
  Polar bumper_polar(0.0, ArtVehicle::front_bumper_px);
  float distance = Euclidean::DistanceToWaypt(bumper_polar, estimate->pos,
					      order->waypt[1]);

  if (distance <= spot_waypoint_radius)
    {
      // The car is near this way-point.
      if (verbose)
	ART_MSG(2, "reached spot waypoint %s, distance %.3fm",
		    order->waypt[1].id.name().str, distance);
      navdata->last_waypt = order->waypt[1].id;
      found = true;
    }
  else
    {
      if (verbose >= 5)
	ART_MSG(2, "distance to spot waypoint %s is %.3fm",
		order->waypt[1].id.name().str, distance);
    }

  return found;
}

float Course::max_speed_for_slow_down(const float& final_speed,
				      const float& distance,
				      const float& max,
				      const float& max_deceleration) {
  // This function answers the question:
  //
  // What is the fastest I could be going right now such that I can be
  // travelling at <final_speed> in <distance> without exceeding 
  // <max_deceleration> between now and then?
  //
  // It uses one of the basic kinematic equations:
  // Vf^2 = Vi^2 + 2 * a * (Xf - Xi)
  
  
  float vf2 = final_speed * final_speed;
  float tax = 2 * (-max_deceleration) * distance;
  
  // Return 0 if it's impossible to stop in time!
  if(tax > vf2)
    return 0.0;
  
  return fminf(max, sqrtf(vf2 - tax));
}

float Course::max_speed_for_change_in_heading(const float& dheading,
					      const float& distance,
					      const float& max,
					      const float& maximum_yaw_rate) {
  // This function answers the question:
  //
  // What is the fastest I could be going right now such that my
  // heading changes <dheading> over the next <distance>, but I never
  // exceed <maximum_yaw_rate>?
  
  // XXX: Include art/epsilon.h and use equal
  if(Epsilon::equal(dheading,0))
    return max;
  else
    {
      float new_speed=fminf(max, fmaxf(max_speed_for_sharp,fabsf(heading_change_ratio * (maximum_yaw_rate / dheading)))); 
      if (verbose>=5)
	ART_MSG(3,"slow for heading: distance: %.3f, dheading: %.3f, maximum_yaw_rate: %.3f, max_speed: %.3f, final: %.3f",distance,dheading,maximum_yaw_rate,max,new_speed); 
      return new_speed;
    }
}


float Course::get_yaw_spring_system(const Polar& aim_polar, 
				    int poly_id,
				    float poly_heading,
				    float max_yaw,
				    float curr_velocity,
				    float offset_ratio)
{
  float error = 0;
  float theta=-aim_polar.heading;
  float velocity = fmaxf(curr_velocity, Steering::steer_speed_min);
  player_position2d_data_t pos_est;  
  
  player_position2d_data_t front_est;  
  Estimate::front_axle_pose(*estimate, front_est);
  double time_in_future=nav->cycle->Time()+velocity*spring_lookahead;
  Estimate::control_pose(front_est,
			 nav->cycle->Time(),
			 time_in_future,
			 pos_est);
 
  if (poly_id >=0)
    {
      poly current_poly=plan.at(poly_id);
      posetype origin;			// (0, 0, 0)
      posetype cpoly(current_poly.midpoint.x, current_poly.midpoint.y,
		     poly_heading);
      rotate_translate_transform trans;
      trans.find_transform(cpoly,origin);
      posetype car(pos_est.pos.px,pos_est.pos.py,0);
      posetype car_rel=trans.apply_transform(car);

      float width=Euclidean::DistanceTo(current_poly.p2,current_poly.p3);

      // transverse offset error, positive if left of center (push right)
      error=car_rel.y;
      //      ART_MSG(1,"STEER error = %lf\n", error);

#if 1 // still experimental:

      if (!Epsilon::equal(offset_ratio, 0.0))
	{
	  // To steer for an offset from lane center, adjust error by
	  // subtracting offset from polygon midpoint to middle of
	  // left lane boundary minus width of car.  

	  MapXY mid_left_side =
	    pops->midpoint(current_poly.p1, current_poly.p2);
	  float half_lane_width =
	    Euclidean::DistanceTo(current_poly.midpoint, mid_left_side);
	  float lane_space = half_lane_width - ArtVehicle::halfwidth;
	  float error_offset = 0.0;
	  if (lane_space > 0.0)		// any room in this lane?
	    error_offset = offset_ratio * lane_space;
	  if (verbose >= 3)
	    ART_MSG(8, "error offset %.3f, half lane width %.3f, ratio %.3f",
		    error_offset, half_lane_width, offset_ratio);

	  // Increasing error term pushes right, decreasing left.
	  error -= error_offset;
	}
#endif
      error=fminf(fmaxf(-width,error),width);
      // heading error
      theta=Coordinates::normalize(pos_est.pos.pa-poly_heading);
    }


  float cth = cosf(theta);

  float vcth = velocity*cth;

  if (fabsf(theta) >= HALFPI ||
      Epsilon::equal(cth,0.0) ||
      Epsilon::equal(vcth,0.0))
    {
      ART_MSG(8,"Spring system does not apply: heading offset %.3f", theta);
      if (Epsilon::equal(error,0)) {
	if (theta < 0)
	  return max_yaw;
	else return -max_yaw;
      }
      else {
	if (error > 0)
	  return max_yaw;
	else return -max_yaw;
      }
    }
  
  float d2=-k_theta*sinf(theta)/cth;
  float d1=-k_error*error/vcth;  
  
// #ifdef NQE
//   if (order->waypt[0].id==ElementID(2,1,3) &&
//       order->waypt[1].id==ElementID(1,1,2))
//     {
//       d2=-0.7*sinf(theta)/cth;
//       ART_MSG(1,"Taking special turn");
//     }
// #endif

  if ((Coordinates::sign(error) == Coordinates::sign(last_error)) &&
      (fabsf(error) > fabs(last_error)))
    d1*=k_int;

  last_error=error;
  float yaw=d1+d2;

  if (verbose >=3)
    ART_MSG(8,"Heading spring systems values: error %.3f, dtheta %.3f, d1 %.3f, d2 %.3f, d1+d2 %.3f",error,theta,d1,d2,yaw);
  
  if (yaw < 0)
    return fmaxf(-max_yaw, yaw);
  return fminf(max_yaw, yaw);
}


bool Course::spot_ahead()
{
  for (uint i=0; i<N_ORDER_WAYPTS-1;i++)
    if (order->waypt[i].is_spot &&
	order->waypt[i+1].is_spot &&
	order->waypt[i].id.pt==1 &&
	order->waypt[i+1].id.pt==2)
      return true;

  return false;

}

bool Course::curr_spot()
{
  return order->waypt[0].is_spot;
}

mapxy_list_t Course::calculate_zone_barrier_points() 
{
  mapxy_list_t spot_points;
  
  return spot_points;

  if (order->waypt[1].is_spot)
    return spot_points;

  posetype way_pose(order->waypt[1].map.x,order->waypt[1].map.y,
		    atan2f(order->waypt[2].map.y-order->waypt[1].map.y,
			   order->waypt[2].map.x-order->waypt[1].map.x));
  
  rotate_translate_transform trans;
  trans.find_transform(posetype(),way_pose);
  
  posetype npose;

  npose=trans.apply_transform(posetype(1,order->waypt[1].lane_width,0));
  spot_points.push_back(npose);

  npose=trans.apply_transform(posetype(1,order->waypt[1].lane_width/2,0));
  spot_points.push_back(npose);
  
  npose=trans.apply_transform(posetype(1,0,0));
  spot_points.push_back(npose);
  
  npose=trans.apply_transform(posetype(1,-order->waypt[1].lane_width/2,0));
  spot_points.push_back(npose);
  
  npose=trans.apply_transform(posetype(1,-order->waypt[1].lane_width,0));
  spot_points.push_back(npose);

  return spot_points;
}


mapxy_list_t Course::calculate_spot_points(const std::vector<WayPointNode>& new_waypts) 
{
  mapxy_list_t spot_points;

  for (uint i=0; i<N_ORDER_WAYPTS-1;i++)
    if (new_waypts[i].is_spot &&
	new_waypts[i+1].is_spot &&
	new_waypts[i].id.pt==1 &&
	new_waypts[i+1].id.pt==2)
      {
	posetype way_pose(new_waypts[i].map.x,new_waypts[i].map.y,
			  atan2f(new_waypts[i+1].map.y-new_waypts[i].map.y,
				 new_waypts[i+1].map.x-new_waypts[i].map.x));

	float dist=Euclidean::DistanceTo(new_waypts[i+1].map,
					 new_waypts[i].map);
	rotate_translate_transform trans;
	trans.find_transform(posetype(),way_pose);
	
	posetype npose;

	npose=trans.apply_transform(posetype(0,new_waypts[i].lane_width/2,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(dist,new_waypts[i].lane_width/2,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(dist+2,new_waypts[i].lane_width,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(dist+2,new_waypts[i].lane_width/2,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(dist+2,0,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(dist+2,-new_waypts[i].lane_width/2,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(dist+2,new_waypts[i].lane_width/2,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(dist,-new_waypts[i].lane_width/2,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(0,-new_waypts[i].lane_width/2,0));
	spot_points.push_back(npose);

      }
  return spot_points;
}

mapxy_list_t Course::calculate_spot_points() 
{
  mapxy_list_t spot_points;

  for (uint i=0; i<N_ORDER_WAYPTS-1;i++)
    if (order->waypt[i].is_spot &&
	order->waypt[i+1].is_spot &&
	order->waypt[i].id.pt==1 &&
	order->waypt[i+1].id.pt==2)
      {
	posetype way_pose(order->waypt[i].map.x,order->waypt[i].map.y,
			  atan2f(order->waypt[i+1].map.y-order->waypt[i].map.y,
				 order->waypt[i+1].map.x-order->waypt[i].map.x));

	float dist=Euclidean::DistanceTo(order->waypt[i+1].map,
					 order->waypt[i].map);
	rotate_translate_transform trans;
	trans.find_transform(posetype(),way_pose);
	
	posetype npose;

	npose=trans.apply_transform(posetype(0,order->waypt[i].lane_width/2,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(dist,order->waypt[i].lane_width/2,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(dist+2,order->waypt[i].lane_width,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(dist+2,order->waypt[i].lane_width/2,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(dist+2,0,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(dist+2,-order->waypt[i].lane_width/2,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(dist+2,order->waypt[i].lane_width/2,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(dist,-order->waypt[i].lane_width/2,0));
	spot_points.push_back(npose);

	npose=trans.apply_transform(posetype(0,-order->waypt[i].lane_width/2,0));
	spot_points.push_back(npose);

      }
  return spot_points;
}


bool Course::nqe_special(int i, int j)
{
#ifdef NQE
  
  typedef struct {
    ElementID start;
    ElementID end;
  } id_pair;
  
  static int num_pair=8;
  
  static id_pair id_table[]=
    {
      //AREA A
      {ElementID(1,1,6),ElementID(41,1,1)},
      {ElementID(1,2,5),ElementID(41,2,1)},
      {ElementID(41,1,7),ElementID(1,1,1)},
      {ElementID(41,2,7),ElementID(1,2,1)},
      //AREA B
      {ElementID(6,1,10),ElementID(5,1,1)},
      {ElementID(5,1,7),ElementID(6,1,1)},
      {ElementID(6,1,4),ElementID(8,2,1)},
      {ElementID(6,1,6),ElementID(7,1,1)},
    };
  
  for (int k=0; k< num_pair; k++)
    if (order->waypt[i].id==id_table[k].start &&
	order->waypt[j].id==id_table[k].end)
      return true;
#endif

  return false;
  
}