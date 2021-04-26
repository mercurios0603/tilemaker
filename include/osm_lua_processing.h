/*! \file */ 
#ifndef _OSM_LUA_PROCESSING_H
#define _OSM_LUA_PROCESSING_H

#include <vector>
#include <string>
#include <sstream>
#include <map>
#include "geomtypes.h"
#include "osm_store.h"
#include "shared_data.h"
#include "output_object.h"
#include "read_pbf.h"
#include "shp_mem_tiles.h"
#include "osm_mem_tiles.h"
#include "attribute_store.h"
#include "helpers.h"

// Lua
extern "C" {
	#include "lua.h"
    #include "lualib.h"
    #include "lauxlib.h"
}

#include "kaguya.hpp"


// FIXME: why is this global ?
extern bool verbose;

/**
	\brief OsmLuaProcessing - converts OSM objects into OutputObjectOsmStore objects.
	
	The input objects are generated by PbfReader. The output objects are sent to OsmMemTiles for storage.

	This class provides a consistent interface for Lua scripts to access.
*/
class OsmLuaProcessing : public PbfReaderOutput { 

public:
	// ----	initialization routines

	OsmLuaProcessing(
        OSMStore const *indexStore, OSMStore &osmStore,
        const class Config &configIn, class LayerDefinition &layers, 
		const std::string &luaFile,
		const class ShpMemTiles &shpMemTiles, 
		class OsmMemTiles &osmMemTiles,
		AttributeStore &attributeStore);
	virtual ~OsmLuaProcessing();

	// ----	Helpers provided for main routine

	// Has this object been assigned to any layers?
	bool empty();

	// Shapefile tag remapping
	bool canRemapShapefiles();
	kaguya::LuaTable newTable();
	virtual kaguya::LuaTable remapAttributes(kaguya::LuaTable& in_table, const std::string &layerName);

	// ----	Data loading methods

	/// \brief We are now processing a significant node
	virtual void setNode(NodeID id, LatpLon node, const tag_map_t &tags);

	/// \brief We are now processing a way
	virtual void setWay(WayID wayId, OSMStore::handle_t handle, const tag_map_t &tags);

	/** \brief We are now processing a relation
	 * (note that we store relations as ways with artificial IDs, and that
	 *  we use decrementing positive IDs to give a bit more space for way IDs)
	 */
	virtual void setRelation(int64_t relationId, OSMStore::handle_t relationHandle, const tag_map_t &tags);

	// ----	Metadata queries called from Lua

	// Get the ID of the current object
	std::string Id() const;

	// Check if there's a value for a given key
	bool Holds(const std::string& key) const;

	// Get an OSM tag for a given key (or return empty string if none)
	std::string Find(const std::string& key) const;

	// ----	Spatial queries called from Lua

	// Find intersecting shapefile layer
	std::vector<std::string> FindIntersecting(const std::string &layerName);
	double AreaIntersecting(const std::string &layerName);
	bool Intersects(const std::string &layerName);
	template <typename GeometryT> double intersectsArea(const std::string &layerName, GeometryT &geom) const;
	template <typename GeometryT> std::vector<uint> intersectsQuery(const std::string &layerName, bool once, GeometryT &geom) const;

	std::vector<std::string> FindCovering(const std::string &layerName);
	bool CoveredBy(const std::string &layerName);
	template <typename GeometryT> std::vector<uint> coveredQuery(const std::string &layerName, bool once, GeometryT &geom) const;
		
	// Returns whether it is closed polygon
	bool IsClosed() const;

	// Returns area
	double Area();
	double multiPolygonArea(const MultiPolygon &mp) const;

	// Returns length
	double Length();

	// ----	Requests from Lua to write this way/node to a vector tile's Layer

    template<class GeometryT>
    void CorrectGeometry(GeometryT &geom)
    {
        geom::correct(geom); // fix wrong orientation
#if BOOST_VERSION >= 105800
        geom::validity_failure_type failure;
        if (isRelation && !geom::is_valid(geom,failure)) {
            if (verbose) std::cout << "Relation " << originalOsmID << " has " << boost_validity_error(failure) << std::endl;
            if (failure==10) return; // too few points
        } else if (isWay && !geom::is_valid(geom,failure)) {
            if (verbose) std::cout << "Way " << originalOsmID << " has " << boost_validity_error(failure) << std::endl;
            if (failure==10) return; // too few points
        }
#endif
    }

	// Add layer
	void Layer(const std::string &layerName, bool area);
	void LayerAsCentroid(const std::string &layerName);
	
	// Set attributes in a vector tile's Attributes table
	void Attribute(const std::string &key, const std::string &val);
	void AttributeWithMinZoom(const std::string &key, const std::string &val, const char minzoom);
	void AttributeNumeric(const std::string &key, const float val);
	void AttributeNumericWithMinZoom(const std::string &key, const float val, const char minzoom);
	void AttributeBoolean(const std::string &key, const bool val);
	void AttributeBooleanWithMinZoom(const std::string &key, const bool val, const char minzoom);
	void MinZoom(const unsigned z);

	// ----	vector_layers metadata entry

	void setVectorLayerMetadata(const uint_least8_t layer, const std::string &key, const uint type);

	std::vector<std::string> GetSignificantNodeKeys();

	// ---- Cached geometries creation

	const Linestring &linestringCached();

	const Polygon &polygonCached();

	const MultiPolygon &multiPolygonCached();

	inline AttributeStore &getAttributeStore() { return attributeStore; }

	void setIndexStore(OSMStore const *indexStore) { this->indexStore = indexStore; }
private:
	/// Internal: clear current cached state
	inline void reset() {
		outputs.clear();
		linestringInited = false;
		polygonInited = false;
		multiPolygonInited = false;
	}

	const inline Point getPoint() {
		return Point(lon/10000000.0,latp/10000000.0);
	}
	
	OSMStore const *indexStore;				// global OSM for reading input
	OSMStore &osmStore;						// global OSM store

	kaguya::State luaState;
	bool supportsRemappingShapefiles;
	const class ShpMemTiles &shpMemTiles;
	class OsmMemTiles &osmMemTiles;
	AttributeStore &attributeStore;			// key/value store

	uint64_t osmID;							///< ID of OSM object (relations have decrementing way IDs)
	int64_t originalOsmID;					///< Original OSM object ID
	WayID newWayID = MAX_WAY_ID;			///< Decrementing new ID for relations
	bool isWay, isRelation, isClosed;		///< Way, node, relation?

	int32_t lon,latp;						///< Node coordinates
	OSMStore::handle_t nodeVecHandle;
	OSMStore::handle_t relationHandle;

	Linestring linestringCache;
	bool linestringInited;
	Polygon polygonCache;
	bool polygonInited;
	MultiPolygon multiPolygonCache;
	bool multiPolygonInited;

	const class Config &config;
	class LayerDefinition &layers;
	
	std::deque<std::pair<OutputObjectRef, AttributeStore::key_value_set_entry_t> > outputs;			///< All output objects that have been created
	boost::container::flat_map<std::string, std::string> currentTags;

};

#endif //_OSM_LUA_PROCESSING_H
