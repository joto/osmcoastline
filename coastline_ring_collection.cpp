/*

  Copyright 2012 Jochen Topf <jochen@topf.org>.

  This file is part of OSMCoastline.

  OSMCoastline is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OSMCoastline is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OSMCoastline.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <iostream>
#include <ogr_geometry.h>

#include "coastline_polygons.hpp"
#include "coastline_ring_collection.hpp"
#include "output_database.hpp"
#include "srs.hpp"

extern SRS srs;
extern bool debug;

CoastlineRingCollection::CoastlineRingCollection() :
    m_list(),
    m_start_nodes(),
    m_end_nodes(),
    m_ways(0),
    m_rings_from_single_way(0),
    m_fixed_rings(0) {
}

/**
 * If a way is not closed adding it to the coastline collection is a bit
 * complicated.
 * We'll check if there is an existing CoastlineRing that our way connects
 * to and add it to that ring. If there is none, we'll create a new
 * CoastlineRing for it and add that to the collection.
 */
void CoastlineRingCollection::add_partial_ring(const shared_ptr<Osmium::OSM::Way>& way) {
    idmap_t::iterator mprev = m_end_nodes.find(way->get_first_node_id());
    idmap_t::iterator mnext = m_start_nodes.find(way->get_last_node_id());

    // There is no CoastlineRing yet where this way could fit. So we
    // create one and add it to the collection.
    if (mprev == m_end_nodes.end() &&
        mnext == m_start_nodes.end()) {
        coastline_rings_list_t::iterator added = m_list.insert(m_list.end(), make_shared<CoastlineRing>(way));
        m_start_nodes[way->get_first_node_id()] = added;
        m_end_nodes[way->get_last_node_id()] = added;
        return;
    }

    // We found a CoastlineRing where we can add the way at the end.
    if (mprev != m_end_nodes.end()) {
        coastline_rings_list_t::iterator prev = mprev->second;
        (*prev)->add_at_end(way);
        m_end_nodes.erase(mprev);

        if ((*prev)->is_closed()) {
            m_start_nodes.erase(m_start_nodes.find((*prev)->first_node_id()));
            return;
        }

        // We also found a CoastlineRing where we could have added the
        // way at the front. This means that the way together with the
        // ring at front and the ring at back are now a complete ring.
        if (mnext != m_start_nodes.end()) {
            coastline_rings_list_t::iterator next = mnext->second;
            (*prev)->join(**next);
            m_start_nodes.erase(mnext);
            if ((*prev)->is_closed()) {
                idmap_t::iterator x = m_start_nodes.find((*prev)->first_node_id());
                if (x != m_start_nodes.end()) {
                    m_start_nodes.erase(x);
                }
                x = m_end_nodes.find((*prev)->last_node_id());
                if (x != m_end_nodes.end()) {
                    m_end_nodes.erase(x);
                }
            }
            m_list.erase(next);
        }

        m_end_nodes[(*prev)->last_node_id()] = prev;
        return;
    }

    // We found a CoastlineRing where we can add the way at the front.
    if (mnext != m_start_nodes.end()) {
        coastline_rings_list_t::iterator next = mnext->second;
        (*next)->add_at_front(way);
        m_start_nodes.erase(mnext);
        if ((*next)->is_closed()) {
            m_end_nodes.erase(m_end_nodes.find((*next)->last_node_id()));
            return;
        }
        m_start_nodes[(*next)->first_node_id()] = next;
    }
}

void CoastlineRingCollection::setup_positions(posmap_t& posmap) {
    for (coastline_rings_list_t::const_iterator it = m_list.begin(); it != m_list.end(); ++it) {
        (*it)->setup_positions(posmap);
    }
}

void CoastlineRingCollection::add_polygons_to_vector(std::vector<OGRGeometry*>& vector) {
    vector.reserve(m_list.size());

    for (coastline_rings_list_t::const_iterator it = m_list.begin(); it != m_list.end(); ++it) {
        CoastlineRing& cp = **it;
        if (cp.is_closed() && cp.npoints() > 3) { // everything that doesn't match here is bad beyond repair and reported elsewhere
            OGRPolygon* p = cp.ogr_polygon(true);
            if (p->IsValid()) {
                p->assignSpatialReference(srs.wgs84());
                vector.push_back(p);
            } else {
                OGRGeometry* geom = p->Buffer(0);
                if (geom && (geom->getGeometryType() == wkbPolygon) && (static_cast<OGRPolygon*>(geom)->getExteriorRing()->getNumPoints() > 3) && (static_cast<OGRPolygon*>(geom)->getNumInteriorRings() == 0) && geom->IsValid()) {
                    geom->assignSpatialReference(srs.wgs84());
                    vector.push_back(static_cast<OGRPolygon*>(geom));
                } else {
                    std::cerr << "Ignoring invalid polygon geometry (ring_id=" << cp.ring_id() << ").\n";
                    delete geom;
                }
                delete p;
            }
        }
    }
}

unsigned int CoastlineRingCollection::output_rings(OutputDatabase& output) {
    unsigned int warnings = 0;

    for (coastline_rings_list_t::const_iterator it = m_list.begin(); it != m_list.end(); ++it) {
        CoastlineRing& cp = **it;
        if (cp.is_closed()) {
            if (cp.npoints() > 3) {
                output.add_ring(cp.ogr_polygon(true), cp.ring_id(), cp.nways(), cp.npoints(), cp.is_fixed());
            } else if (cp.npoints() == 1) {
                output.add_error_point(cp.ogr_first_point(), "single_point_in_ring", cp.first_node_id());
                warnings++;
            } else { // cp.npoints() == 2 or 3
                output.add_error_line(cp.ogr_linestring(true), "not_a_ring", cp.ring_id());
                output.add_error_point(cp.ogr_first_point(), "not_a_ring", cp.first_node_id());
                output.add_error_point(cp.ogr_last_point(), "not_a_ring", cp.last_node_id());
                warnings++;
            }
        } else {
            output.add_error_line(cp.ogr_linestring(true), "not_closed", cp.ring_id());
            output.add_error_point(cp.ogr_first_point(), "end_point", cp.first_node_id());
            output.add_error_point(cp.ogr_last_point(), "end_point", cp.last_node_id());
            warnings++;
        }
    }

    return warnings;
}

Osmium::OSM::Position intersection(const Osmium::OSM::Segment& s1, const Osmium::OSM::Segment&s2) {
    if (s1.first()  == s2.first()  ||
        s1.first()  == s2.second() ||
        s1.second() == s2.first()  ||
        s1.second() == s2.second()) {
        return Osmium::OSM::Position();
    }

    double denom = ((s2.second().lat() - s2.first().lat())*(s1.second().lon() - s1.first().lon())) -
                   ((s2.second().lon() - s2.first().lon())*(s1.second().lat() - s1.first().lat()));

    if (denom != 0) {
        double nume_a = ((s2.second().lon() - s2.first().lon())*(s1.first().lat() - s2.first().lat())) -
                        ((s2.second().lat() - s2.first().lat())*(s1.first().lon() - s2.first().lon()));

        double nume_b = ((s1.second().lon() - s1.first().lon())*(s1.first().lat() - s2.first().lat())) -
                        ((s1.second().lat() - s1.first().lat())*(s1.first().lon() - s2.first().lon()));

        if ((denom > 0 && nume_a >= 0 && nume_a <= denom && nume_b >= 0 && nume_b <= denom) ||
            (denom < 0 && nume_a <= 0 && nume_a >= denom && nume_b <= 0 && nume_b >= denom)) {
            double ua = nume_a / denom;
            double ix = s1.first().lon() + ua*(s1.second().lon() - s1.first().lon());
            double iy = s1.first().lat() + ua*(s1.second().lat() - s1.first().lat());
            return Osmium::OSM::Position(ix, iy);
        }
    }

    return Osmium::OSM::Position();
}

bool outside_x_range(const Osmium::OSM::UndirectedSegment& s1, const Osmium::OSM::UndirectedSegment& s2) {
    if (s1.first().x() > s2.second().x()) {
        return true;
    }
    return false;
}

bool y_range_overlap(const Osmium::OSM::UndirectedSegment& s1, const Osmium::OSM::UndirectedSegment& s2) {
    int tmin = s1.first().y() < s1.second().y() ? s1.first().y( ) : s1.second().y();
    int tmax = s1.first().y() < s1.second().y() ? s1.second().y() : s1.first().y();
    int omin = s2.first().y() < s2.second().y() ? s2.first().y()  : s2.second().y();
    int omax = s2.first().y() < s2.second().y() ? s2.second().y() : s2.first().y();
    if (tmin > omax || omin > tmax) {
        return false;
    }
    return true;
}

OGRLineString* create_ogr_linestring(const Osmium::OSM::Segment& segment) {
    OGRLineString* line = new OGRLineString;
    line->setNumPoints(2);
    line->setPoint(0, segment.first().lon(), segment.first().lat());
    line->setPoint(1, segment.second().lon(), segment.second().lat());
    line->setCoordinateDimension(2);

    return line;
}

/**
 * Checks if there are intersections between any coastline segments.
 * Returns the number of intersections and overlaps.
 */
unsigned int CoastlineRingCollection::check_for_intersections(OutputDatabase& output) {
    unsigned int overlaps = 0;

    std::vector<Osmium::OSM::UndirectedSegment> segments;
    if (debug) std::cerr << "Setting up segments...\n";
    for (coastline_rings_list_t::const_iterator it = m_list.begin(); it != m_list.end(); ++it) {
        it->get()->add_segments_to_vector(segments);
    }

    if (debug) std::cerr << "Sorting...\n";
    std::sort(segments.begin(), segments.end());

    if (debug) std::cerr << "Finding intersections...\n";
    std::vector<Osmium::OSM::Position> intersections;
    for (std::vector<Osmium::OSM::UndirectedSegment>::const_iterator it1 = segments.begin(); it1 != segments.end()-1; ++it1) {
        const Osmium::OSM::UndirectedSegment& s1 = *it1;
        for (std::vector<Osmium::OSM::UndirectedSegment>::const_iterator it2 = it1+1; it2 != segments.end(); ++it2) {
            const Osmium::OSM::UndirectedSegment& s2 = *it2;
            if (s1 == s2) {
                OGRLineString* line = create_ogr_linestring(s1);
                output.add_error_line(line, "overlap");
                overlaps++;
            } else {
                if (outside_x_range(s2, s1)) {
                    break;
                }
                if (y_range_overlap(s1, s2)) {
                    Osmium::OSM::Position i = intersection(s1, s2);
                    if (i.defined()) {
                        intersections.push_back(i);
                    }
                }
            }
        }
    }

    for (std::vector<Osmium::OSM::Position>::const_iterator it = intersections.begin(); it != intersections.end(); ++it) {
        OGRPoint* point = new OGRPoint(it->lon(), it->lat());
        output.add_error_point(point, "intersection");
    }

    return intersections.size() + overlaps;
}

void CoastlineRingCollection::close_rings(OutputDatabase& output, bool debug, double max_distance) {
    std::vector<Connection> connections;

    // Create vector with all possible combinations of connections between rings.
    for (idmap_t::iterator eit = m_end_nodes.begin(); eit != m_end_nodes.end(); ++eit) {
        for (idmap_t::iterator sit = m_start_nodes.begin(); sit != m_start_nodes.end(); ++sit) {
            double distance = (*sit->second)->distance_to_start_position((*eit->second)->last_position());
            if (distance < max_distance) {
                connections.push_back(Connection(distance, eit->first, sit->first));
            }
        }
    }

    // Sort vector by distance, shortest at end.
    std::sort(connections.begin(), connections.end(), Connection::sort_by_distance);

    // Go through vector starting with the shortest connections and close rings
    // using the connections in turn.
    while (!connections.empty()) {
        Connection conn = connections.back();
        connections.pop_back();

        // Invalidate all other connections using one of the same end points.
        connections.erase(remove_if(connections.begin(), connections.end(), conn), connections.end());

        idmap_t::iterator eit = m_end_nodes.find(conn.start_id);
        idmap_t::iterator sit = m_start_nodes.find(conn.end_id);

        if (eit != m_end_nodes.end() && sit != m_start_nodes.end()) {
            if (debug) {
                std::cerr << "Closing ring between node " << conn.end_id << " and node " << conn.start_id << "\n";
            }

            m_fixed_rings++;

            CoastlineRing* e = eit->second->get();
            CoastlineRing* s = sit->second->get();

            output.add_error_point(e->ogr_last_point(), "fixed_end_point", e->last_node_id());
            output.add_error_point(s->ogr_first_point(), "fixed_end_point", s->first_node_id());

            if (e->last_position() != s->first_position()) {
                OGRLineString* linestring = new OGRLineString;
                linestring->addPoint(e->last_position().lon(), e->last_position().lat());
                linestring->addPoint(s->first_position().lon(), s->first_position().lat());
                output.add_error_line(linestring, "added_line");
            }

            if (e == s) {
                // connect to itself by closing ring
                e->close_ring();

                m_end_nodes.erase(eit);
                m_start_nodes.erase(sit);
            } else {
                // connect to other ring
                e->join_over_gap(*s);

                m_list.erase(sit->second);
                if (e->first_position() == e->last_position()) {
                    output.add_error_point(e->ogr_first_point(), "double_node", e->first_node_id());
                    m_start_nodes.erase(e->first_node_id());
                    m_end_nodes.erase(eit);
                    m_start_nodes.erase(sit);
                    m_end_nodes.erase(e->last_node_id());
                    e->fake_close();
                } else {
                    m_end_nodes[e->last_node_id()] = eit->second;
                    m_end_nodes.erase(eit);
                    m_start_nodes.erase(sit);
                }
            }
        }
    }
}

/**
 * Finds some questionably polygons. This will find
 * a) some polygons touching another polygon in a single point
 * b) holes inside land (those should usually be tagged as water, riverbank, or so, not as coastline)
 *    very large such objects will not be reported, this excludes the Great Lakes etc.
 * c) holes inside holes (those are definitely wrong)
 *
 * Returns the number of warnings.
 */
unsigned int CoastlineRingCollection::output_questionable(const CoastlinePolygons& polygons, OutputDatabase& output) {
    const unsigned int max_nodes_to_be_considered_questionable = 1000;
    unsigned int warnings = 0;

    typedef std::pair<Osmium::OSM::Position, CoastlineRing*> pos_ring_ptr_t;
    std::vector<pos_ring_ptr_t> rings;
    rings.reserve(m_list.size());

    // put all rings in a vector...
    for (coastline_rings_list_t::const_iterator it = m_list.begin(); it != m_list.end(); ++it) {
        rings.push_back(std::make_pair<Osmium::OSM::Position, CoastlineRing*>((*it)->first_position(), it->get()));
    }

    // ... and sort it by position of the first node in the ring (this allows binary search in it)
    std::sort(rings.begin(), rings.end());

    // go through all the polygons that have been created before and mark the outer rings
    for (polygon_vector_t::const_iterator it = polygons.begin(); it != polygons.end(); ++it) {
        OGRLinearRing* exterior_ring = (*it)->getExteriorRing();
        Osmium::OSM::Position pos(exterior_ring->getX(0), exterior_ring->getY(0));
        std::vector<pos_ring_ptr_t>::iterator rings_it = lower_bound(rings.begin(), rings.end(), std::make_pair<Osmium::OSM::Position, CoastlineRing*>(pos, NULL));
        if (rings_it != rings.end()) {
            rings_it->second->set_outer();
        }
    }

    // find all rings not marked as outer and output them to the error_lines table
    for (coastline_rings_list_t::const_iterator it = m_list.begin(); it != m_list.end(); ++it) {
        if (!(*it)->is_outer()) {
            CoastlineRing& ring = **it;
            if (ring.is_closed() && ring.npoints() > 3 && ring.npoints() < max_nodes_to_be_considered_questionable) {
                output.add_error_line(ring.ogr_linestring(false), "questionable", ring.ring_id());
                warnings++;
            }
        }
    }

    return warnings;
}
