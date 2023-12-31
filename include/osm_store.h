/*! \file */ 
#ifndef _OSM_STORE_H
#define _OSM_STORE_H

#include "geom.h"
#include "coordinates.h"
#include "mmap_allocator.h"

#include <utility>
#include <vector>
#include <mutex>
#include <unordered_set>
#include <boost/container/flat_map.hpp>

extern bool verbose;

class NodeStore;
class WayStore;

//
// Internal data structures.
//
// list of ways used by relations
// by noting these in advance, we don't need to store all ways in the store
class UsedWays {

private:
	std::vector<bool> usedList;
	mutable std::mutex mutex;

public:
	bool inited = false;

	// Size the vector to a reasonable estimate, to avoid resizing on the fly
	// TODO: it'd be preferable if UsedWays didn't know about compact mode --
	//   instead, use an efficient data structure if numNodes < 1B, otherwise
	//   use a large bitvector
	void reserve(bool compact, size_t numNodes) {
		std::lock_guard<std::mutex> lock(mutex);
		if (inited) return;
		inited = true;
		if (compact) {
			// If we're running in compact mode, way count is roughly 1/9th of node count... say 1/8 to be safe
			usedList.reserve(numNodes/8);
		} else {
			// Otherwise, we could have anything up to the current max node ID (approaching 2**30 in summer 2021)
			// 2**31 is 0.25GB with a vector<bool>
			usedList.reserve(pow(2,31));
		}
	}
	
	// Mark a way as used
	void insert(WayID wayid) {
		std::lock_guard<std::mutex> lock(mutex);
		if (wayid>usedList.size()) usedList.resize(wayid+256);
		usedList[wayid] = true;
	}
	
	// See if a way is used
	bool at(WayID wayid) const {
		return (wayid>usedList.size()) ? false : usedList[wayid];
	}
	
	void clear() {
		std::lock_guard<std::mutex> lock(mutex);
		usedList.clear();
	}
};

// scanned relations store
class RelationScanStore {

private:
	using tag_map_t = boost::container::flat_map<std::string, std::string>;
	std::map<WayID, std::vector<WayID>> relationsForWays;
	std::map<WayID, tag_map_t> relationTags;
	mutable std::mutex mutex;

public:
	void relation_contains_way(WayID relid, WayID wayid) {
		std::lock_guard<std::mutex> lock(mutex);
		relationsForWays[wayid].emplace_back(relid);
	}
	void store_relation_tags(WayID relid, const tag_map_t &tags) {
		std::lock_guard<std::mutex> lock(mutex);
		relationTags[relid] = tags;
	}
	bool way_in_any_relations(WayID wayid) {
		return relationsForWays.find(wayid) != relationsForWays.end();
	}
	std::vector<WayID> relations_for_way(WayID wayid) {
		return relationsForWays[wayid];
	}
	std::string get_relation_tag(WayID relid, const std::string &key) {
		auto it = relationTags.find(relid);
		if (it==relationTags.end()) return "";
		auto jt = it->second.find(key);
		if (jt==it->second.end()) return "";
		return jt->second;
	}
	void clear() {
		std::lock_guard<std::mutex> lock(mutex);
		relationsForWays.clear();
		relationTags.clear();
	}
};


// relation store
// (this isn't currently used as we don't need to store relations for later processing, but may be needed for nested relations)

class RelationStore {

public:	
	using wayid_vector_t = std::vector<WayID, mmap_allocator<NodeID>>;
	using relation_entry_t = std::pair<wayid_vector_t, wayid_vector_t>;

	using element_t = std::pair<WayID, relation_entry_t>;
	using map_t = std::deque<element_t, mmap_allocator<element_t>>;

	void reopen() {
		std::lock_guard<std::mutex> lock(mutex);
		mOutInLists = std::make_unique<map_t>();
	}

	// @brief Insert a list of relations
	void insert_front(std::vector<element_t> &new_relations) {
		std::lock_guard<std::mutex> lock(mutex);
		auto i = mOutInLists->size();
		mOutInLists->resize(i + new_relations.size());
		std::copy(std::make_move_iterator(new_relations.begin()), std::make_move_iterator(new_relations.end()), mOutInLists->begin() + i); 
	}

	// @brief Make the store empty
	void clear() {
		std::lock_guard<std::mutex> lock(mutex);
		mOutInLists->clear();
	}

	std::size_t size() const {
		std::lock_guard<std::mutex> lock(mutex);
		return mOutInLists->size(); 
	}

private: 	
	mutable std::mutex mutex;
	std::unique_ptr<map_t> mOutInLists;
};

/**
	\brief OSM store keeps nodes, ways and relations in memory for later access

	Store all of those to be output: latp/lon for nodes, node list for ways, and way list for relations.
	It will serve as the global data store. OSM data destined for output will be set here from OsmMemTiles.

	Internal data structures are encapsulated in NodeStore, WayStore and RelationStore classes.
	These store can be altered for efficient memory use without global code changes.
	Such data structures have to return const ForwardInputIterators (only *, ++ and == should be supported).

	Possible future improvements to save memory:
	- pack WayStore (e.g. zigzag PBF encoding and varint)
	- combine innerWays and outerWays into one vector, with a single-byte index marking the changeover
	- use two arrays (sorted keys and elements) instead of map
*/
class OSMStore
{
public:
	NodeStore& nodes;
	WayStore& ways;

protected:	
	bool use_compact_nodes = false;
	bool require_integrity = true;

	RelationStore relations; // unused
	UsedWays used_ways;
	RelationScanStore scanned_relations;

public:

	OSMStore(NodeStore& nodes, WayStore& ways): nodes(nodes), ways(ways)
	{ 
		reopen();
	}

	void reopen();

	void open(std::string const &osm_store_filename);

	void use_compact_store(bool use) { use_compact_nodes = use; }
	void enforce_integrity(bool ei) { require_integrity = ei; }
	bool integrity_enforced() { return require_integrity; }

	void relations_insert_front(std::vector<RelationStore::element_t> &new_relations) {
		relations.insert_front(new_relations);
	}
	void relations_sort(unsigned int threadNum);

	void mark_way_used(WayID i) { used_ways.insert(i); }
	bool way_is_used(WayID i) { return used_ways.at(i); }

	void ensureUsedWaysInited();

	using tag_map_t = boost::container::flat_map<std::string, std::string>;
	void relation_contains_way(WayID relid, WayID wayid) { scanned_relations.relation_contains_way(relid,wayid); }
	void store_relation_tags(WayID relid, const tag_map_t &tags) { scanned_relations.store_relation_tags(relid,tags); }
	bool way_in_any_relations(WayID wayid) { return scanned_relations.way_in_any_relations(wayid); }
	std::vector<WayID> relations_for_way(WayID wayid) { return scanned_relations.relations_for_way(wayid); }
	std::string get_relation_tag(WayID relid, const std::string &key) { return scanned_relations.get_relation_tag(relid, key); }

	void clear();
	void reportSize() const;

	// Relation -> MultiPolygon or MultiLinestring
	MultiPolygon wayListMultiPolygon(WayVec::const_iterator outerBegin, WayVec::const_iterator outerEnd, WayVec::const_iterator innerBegin, WayVec::const_iterator innerEnd) const;
	MultiLinestring wayListMultiLinestring(WayVec::const_iterator outerBegin, WayVec::const_iterator outerEnd) const;
	void mergeMultiPolygonWays(std::vector<LatpLonDeque> &results, std::map<WayID,bool> &done, WayVec::const_iterator itBegin, WayVec::const_iterator itEnd) const;

	///It is not really meaningful to try using a relation as a linestring. Not normally used but included
	///if Lua script attempts to do this.
	//
	// Relation -> MultiPolygon
	static Linestring wayListLinestring(MultiPolygon const &mp) {
		Linestring out;
		if(!mp.empty()) {
			for(auto pt: mp[0].outer())
				boost::geometry::append(out, pt);
		}
		return out;
	}

	template<class WayIt>
	Polygon llListPolygon(WayIt begin, WayIt end) const {
		Polygon poly;
		fillPoints(poly.outer(), begin, end);
		boost::geometry::correct(poly);
		return poly;
	}

	// Way -> Linestring
	template<class WayIt>
	Linestring llListLinestring(WayIt begin, WayIt end) const {
		Linestring ls;
		fillPoints(ls, begin, end);
		return ls;
	}

private:
	// helper
	template<class PointRange, class LatpLonIt>
	void fillPoints(PointRange &points, LatpLonIt begin, LatpLonIt end) const {
		for (auto it = begin; it != end; ++it) {
			try {
				boost::geometry::range::push_back(points, boost::geometry::make<Point>(it->lon/10000000.0, it->latp/10000000.0));
			} catch (std::out_of_range &err) {
				if (require_integrity) throw err;
			}
		}
	}
};

#endif //_OSM_STORE_H
