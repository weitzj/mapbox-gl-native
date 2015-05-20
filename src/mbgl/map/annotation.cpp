#include <mbgl/map/annotation.hpp>
#include <mbgl/map/map.hpp>
#include <mbgl/map/tile_id.hpp>
#include <mbgl/map/live_tile.hpp>
#include <mbgl/map/map_data.hpp>
#include <mbgl/util/constants.hpp>
#include <mbgl/util/ptr.hpp>
#include <mbgl/util/std.hpp>

#include <algorithm>
#include <memory>

namespace mbgl {

class Annotation : private util::noncopyable {
    friend class AnnotationManager;
public:
    Annotation(AnnotationType, const AnnotationSegments&);

private:
    LatLng getPoint() const;
    LatLngBounds getBounds() const { return bounds; }

private:
    const AnnotationType type = AnnotationType::Point;
    const AnnotationSegments geometry;
    std::unordered_map<TileID, std::weak_ptr<const LiveTileFeature>, TileID::Hash> tileFeatures;
    const LatLngBounds bounds;
};

Annotation::Annotation(AnnotationType type_, const AnnotationSegments& geometry_)
    : type(type_),
      geometry(geometry_),
      bounds([this] {
          LatLngBounds bounds_;
          if (type == AnnotationType::Point) {
              bounds_ = { getPoint(), getPoint() };
          } else {
              for (auto& segment : geometry) {
                  for (auto& point : segment) {
                      bounds_.extend(point);
                  }
              }
          }
          return bounds_;
      }()) {
}

LatLng Annotation::getPoint() const {
    // Return the first line's first point. Shortcut for point annotations.
    assert(!geometry.empty());
    assert(!geometry[0].empty());
    return geometry[0][0];
}

AnnotationManager::AnnotationManager() {}

AnnotationManager::~AnnotationManager() {
    // leave this here because the header file doesn't have a definition of
    // Annotation so we can't destruct the object with just the header file.
}

void AnnotationManager::setDefaultPointAnnotationSymbol(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(mtx);
    defaultPointAnnotationSymbol = symbol;
}

uint32_t AnnotationManager::nextID() {
    return nextID_++;
}

vec2<double> AnnotationManager::projectPoint(const LatLng& point) {
    // Project a coordinate into unit space in a square map.
    const double sine = std::sin(point.latitude * M_PI / 180.0);
    const double x = point.longitude / 360.0 + 0.5;
    const double y = 0.5 - 0.25 * std::log((1.0 + sine) / (1.0 - sine)) / M_PI;
    return { x, y };
}

std::pair<std::vector<TileID>, AnnotationIDs>
AnnotationManager::addAnnotations(const AnnotationType type,
                                  const std::vector<AnnotationSegments>& segments,
                                  const AnnotationsProperties& properties,
                                  const MapData& data) {
    std::lock_guard<std::mutex> lock(mtx);

    // We pre-generate tiles to contain each annotation up to the map's max zoom.
    // We do this for fast rendering without projection conversions on the fly, as well as
    // to simplify bounding box queries of annotations later. Tiles get invalidated when
    // annotations are added or removed in order to refresh the map render without
    // touching the base map underneath.

    const uint16_t extent = 4096;

    AnnotationIDs annotationIDs;
    annotationIDs.reserve((type == AnnotationType::Shape ? segments.size() : segments[0][0].size()));

    std::vector<TileID> affectedTiles;

    for (size_t s = 0; s < segments.size(); ++s) {
        auto& shape = segments[s];

        const uint32_t annotationID = nextID();

        // track the annotation global ID and its geometry
        auto anno_it = annotations.emplace(annotationID,
            util::make_unique<Annotation>(type, shape));

        for (size_t l = 0; l < segments[s].size(); ++l) {
            auto& line = shape[l];

            for (size_t p = 0; p < line.size(); ++p) {
                auto& point = line[p];

                const uint8_t maxZoom = data.transform.getMaxZoom();

                // side length of map at this zoom
                uint32_t z2 = 1 << maxZoom;

                // projection conversion into unit space
                const vec2<double> pp = projectPoint(point);

                uint32_t x = pp.x * z2;
                uint32_t y = pp.y * z2;

                for (int8_t z = maxZoom; z >= 0; z--) {
                    affectedTiles.emplace_back(z, x, y);
                    TileID tileID = affectedTiles.back();

                    // calculate tile coordinate
                    const Coordinate coordinate(extent * (pp.x * z2 - x), extent * (pp.y * z2 - y));

                    const GeometryCollection geometries({ { { { coordinate } } } });

                    std::unordered_map<std::string, std::string> featureProperties;

                    if (type == AnnotationType::Point) {
                        // at render time we style the point according to its {sprite} field
                        const std::string& symbol = properties.at("symbols")[p];
                        if (symbol.length()) {
                            featureProperties.emplace("sprite", symbol);
                        } else {
                            featureProperties.emplace("sprite", defaultPointAnnotationSymbol);
                        }
                    } else {

                    }

                    auto feature =
                        std::make_shared<const LiveTileFeature>(
                            (type == AnnotationType::Point ? FeatureType::Point : FeatureType::LineString),
                            geometries,
                            featureProperties);

                    auto tile_it = tiles.find(tileID);
                    if (tile_it != tiles.end()) {
                        //
                        // We have this tile created already. Add this feature to it.
                        //
                        // get point layer & add feature
                        auto layer =
                            tile_it->second.second->getMutableLayer(
                                (type == AnnotationType::Point ? PointLayerID : ShapeLayerID)
                            );
                        layer->addFeature(feature);
                        // record annotation association with tile
                        tile_it->second.first.insert(annotationID);
                    } else {
                        //
                        // We need to create a new tile for this feature.
                        //
                        // create point layer & add feature
                        util::ptr<LiveTileLayer> layer = std::make_shared<LiveTileLayer>();
                        layer->addFeature(feature);
                        // create tile & record annotation association
                        auto tile_pos = tiles.emplace(
                            tileID, std::make_pair(std::unordered_set<uint32_t>({ annotationID }),
                                                   util::make_unique<LiveTile>()));
                        // add point layer to tile
                        tile_pos.first->second.second->addLayer(
                            (type == AnnotationType::Point ? PointLayerID : ShapeLayerID),
                            layer
                        );
                    }

                    // Record annotation association with tile and tile feature. This is used to determine stale tiles,
                    // as well as to remove the feature from the tile upon annotation deletion.
                    anno_it.first->second->tileFeatures.emplace(
                        tileID, std::weak_ptr<const LiveTileFeature>(feature));

                    // get ready for the next-lower zoom number
                    z2 /= 2;
                    x /= 2;
                    y /= 2;
                }
            }
        }

        annotationIDs.push_back(annotationID);
    }

    // Tile:IDs that need refreshed and the annotation identifiers held onto by the client.
    return std::make_pair(affectedTiles, annotationIDs);
}

std::pair<std::vector<TileID>, AnnotationIDs>
AnnotationManager::addPointAnnotations(const AnnotationSegment& points,
                                       const AnnotationsProperties& properties,
                                       const MapData& data) {
    return addAnnotations(AnnotationType::Point,
                          { { points } },
                          properties,
                          data);
}

std::pair<std::vector<TileID>, AnnotationIDs>
AnnotationManager::addShapeAnnotations(const std::vector<AnnotationSegments>& shapes,
                                       const AnnotationsProperties& properties,
                                       const MapData& data) {
    return addAnnotations(AnnotationType::Shape,
                          shapes,
                          properties,
                          data);
}

std::vector<TileID> AnnotationManager::removeAnnotations(const AnnotationIDs& ids,
                                                         const MapData& data) {
    std::lock_guard<std::mutex> lock(mtx);

    std::vector<TileID> affectedTiles;

    std::vector<uint32_t> z2s;
    const uint8_t zoomCount = data.transform.getMaxZoom() + 1;
    z2s.reserve(zoomCount);
    for (uint8_t z = 0; z < zoomCount; ++z) {
        z2s.emplace_back(1<< z);
    }

    LatLng latLng;
    vec2<double> p;
    uint32_t x, y;

    // iterate annotation id's passed
    for (const auto& annotationID : ids) {
        // grab annotation object
        const auto& annotation_it = annotations.find(annotationID);
        if (annotation_it != annotations.end()) {
            const auto& annotation = annotation_it->second;
            // calculate annotation's affected tile for each zoom
            for (uint8_t z = 0; z < zoomCount; ++z) {
                latLng = annotation->getPoint();
                p = projectPoint(latLng);
                x = z2s[z] * p.x;
                y = z2s[z] * p.y;
                TileID tid(z, x, y);
                // erase annotation from tile's list
                auto& tileAnnotations = tiles[tid].first;
                tileAnnotations.erase(annotationID);
                // remove annotation's features from tile
                const auto& features_it = annotation->tileFeatures.find(tid);
                if (features_it != annotation->tileFeatures.end()) {
                    const auto& layer =
                        tiles[tid].second->getMutableLayer(PointLayerID); //
                    layer->removeFeature(features_it->second);
                    affectedTiles.push_back(tid);
                }
            }
            annotations.erase(annotationID);
        }
    }

    // TileIDs for tiles that need refreshed.
    return affectedTiles;
}

AnnotationIDs AnnotationManager::getAnnotationsInBounds(const LatLngBounds& queryBounds,
                                                        const MapData& data) const {
    std::lock_guard<std::mutex> lock(mtx);

    const uint8_t z = data.transform.getMaxZoom();
    const uint32_t z2 = 1 << z;
    const vec2<double> swPoint = projectPoint(queryBounds.sw);
    const vec2<double> nePoint = projectPoint(queryBounds.ne);

    // tiles number y from top down
    const TileID nwTile(z, swPoint.x * z2, nePoint.y * z2);
    const TileID seTile(z, nePoint.x * z2, swPoint.y * z2);

    AnnotationIDs matchingAnnotations;

    for (auto& tile : tiles) {
        TileID id = tile.first;
        if (id.z == z) {
            if (id.x >= nwTile.x && id.x <= seTile.x && id.y >= nwTile.y && id.y <= seTile.y) {
                if (id.x > nwTile.x && id.x < seTile.x && id.y > nwTile.y && id.y < seTile.y) {
                    // Trivial accept; this tile is completely inside the query bounds, so
                    // we'll return all of its annotations.
                    std::copy(tile.second.first.begin(), tile.second.first.end(),
                              std::back_inserter(matchingAnnotations));
                } else {
                    // This tile is intersected by the query bounds. We need to check the
                    // tile's annotations' bounding boxes individually.
                    std::copy_if(tile.second.first.begin(), tile.second.first.end(),
                                 std::back_inserter(matchingAnnotations),
                                 [&](const uint32_t annotationID) -> bool {
                        const auto it = annotations.find(annotationID);
                        if (it != annotations.end()) {
                            const LatLngBounds annoBounds = it->second->getBounds();
                            return (annoBounds.sw.latitude >= queryBounds.sw.latitude &&
                                    annoBounds.ne.latitude <= queryBounds.ne.latitude &&
                                    annoBounds.sw.longitude >= queryBounds.sw.longitude &&
                                    annoBounds.ne.longitude <= queryBounds.ne.longitude);
                        } else {
                            return false;
                        }
                    });
                }
            }
        }
    }

    return matchingAnnotations;
}

LatLngBounds AnnotationManager::getBoundsForAnnotations(const AnnotationIDs& ids) const {
    std::lock_guard<std::mutex> lock(mtx);

    LatLngBounds bounds;
    for (auto id : ids) {
        const auto annotation_it = annotations.find(id);
        if (annotation_it != annotations.end()) {
            bounds.extend(annotation_it->second->getPoint());
        }
    }

    return bounds;
}

const LiveTile* AnnotationManager::getTile(const TileID& id) {
    std::lock_guard<std::mutex> lock(mtx);

    const auto tile_it = tiles.find(id);
    if (tile_it != tiles.end()) {
        return tile_it->second.second.get();
    }
    return nullptr;
}

const std::string AnnotationManager::PointLayerID = "com.mapbox.annotations.points";
const std::string AnnotationManager::ShapeLayerID = "com.mapbox.annotations.shape";

}
