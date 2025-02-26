#include "routing/routing_node.h"

#include "geom/msg.h"
#include "geom/distance.h"

namespace truck::routing {

using namespace std::placeholders;
using namespace std::chrono_literals;

namespace {

RTreePoint toRTreePoint(const geom::Vec2& point) { return RTreePoint(point.x, point.y); }

RTreeIndexedPoint toRTreeIndexedPoint(const geom::Vec2& point, size_t index) {
    return RTreeIndexedPoint(toRTreePoint(point), index);
}

RTree toRTree(const std::vector<geom::Vec2>& points) {
    RTree rtree;

    for (size_t i = 0; i < points.size(); i++) {
        rtree.insert(toRTreeIndexedPoint(points[i], i));
    }

    return rtree;
}

RTree toRTree(const navigation::graph::Nodes& nodes) {
    RTree rtree;

    for (const auto& node : nodes) {
        rtree.insert(toRTreeIndexedPoint(node.point, node.id));
    }

    return rtree;
}

size_t findNearestIndex(const RTree& rtree, const geom::Vec2& point) {
    RTreeIndexedPoints rtree_indexed_points;

    rtree.query(
        bg::index::nearest(toRTreePoint(point), 1), std::back_inserter(rtree_indexed_points));

    return rtree_indexed_points[0].second;
}

}  // namespace

Route::Route() {}

Route::Route(const geom::Polyline& polyline) : polyline(polyline), rtree(toRTree(polyline)) {}

double Route::distance(const geom::Vec2& point) const {
    return geom::distance(point, polyline[findNearestIndex(rtree, point)]);
}

size_t Route::postfixIndex(const geom::Vec2& point, double postfix) const {
    size_t index = findNearestIndex(rtree, point);
    double cur_postfix = 0.0;

    while (cur_postfix < postfix && index > 0) {
        cur_postfix += geom::distance(polyline[index], polyline[index - 1]);
        index--;
    }

    return index;
}

Cache::Cache() {}

Cache::Cache(const navigation::graph::Graph& graph) : graph(graph), rtree(toRTree(graph.nodes)) {}

geom::Polyline Cache::findPath(const geom::Vec2& from, const geom::Vec2& to) const {
    navigation::graph::NodeId from_id = findNearestIndex(rtree, from);
    navigation::graph::NodeId to_id = findNearestIndex(rtree, to);

    return navigation::search::toPolyline(
        graph, navigation::search::findShortestPath(graph, from_id, to_id));
}

RoutingNode::RoutingNode() : Node("routing") {
    initializeParams();
    initializeTopicHandlers();

    map_ = std::make_unique<map::Map>(map::Map::fromGeoJson(params_.map_config));
    mesh_builder_ = std::make_unique<navigation::mesh::MeshBuilder>(params_.mesh);
    graph_builder_ = std::make_unique<navigation::graph::GraphBuilder>(params_.graph);

    cache_ =
        Cache(graph_builder_->build(mesh_builder_->build(map_->polygons()).mesh, map_->polygons()));
}

void RoutingNode::initializeParams() {
    params_.map_config = this->declare_parameter<std::string>("map_config");

    params_.route = Params::Route{
        .max_ego_dist = this->declare_parameter<double>("route.max_ego_dist"),
        .postfix_len = this->declare_parameter<double>("route.postfix_len"),
        .spline_step = this->declare_parameter<double>("route.spline_step")};

    params_.mesh = navigation::mesh::MeshParams{
        .dist = this->declare_parameter<double>("mesh.dist"),
        .offset = this->declare_parameter<double>("mesh.offset"),
        .filter = {}};

    bool k_nearest_mode = this->declare_parameter<bool>("graph.k_nearest_mode");
    auto mode = (k_nearest_mode == true) ? navigation::graph::GraphParams::Mode::kNearest
                                         : navigation::graph::GraphParams::Mode::searchRadius;

    params_.graph = {
        .mode = mode,
        .k_nearest = static_cast<size_t>(this->declare_parameter<int>("graph.k_nearest")),
        .search_radius = this->declare_parameter<double>("graph.search_radius")};

    RCLCPP_INFO(this->get_logger(), "route max_ego_dist: %.2f m", params_.route.max_ego_dist);
    RCLCPP_INFO(this->get_logger(), "route postfix_len: %.2f m", params_.route.postfix_len);
    RCLCPP_INFO(this->get_logger(), "route spline_step: %.2f m", params_.route.spline_step);

    RCLCPP_INFO(this->get_logger(), "mesh dist: %.2f m", params_.mesh.dist);
    RCLCPP_INFO(this->get_logger(), "mesh offset: %.2f m", params_.mesh.offset);

    if (k_nearest_mode == true) {
        RCLCPP_INFO(this->get_logger(), "graph k_nearest: %li", params_.graph.k_nearest);
    } else {
        RCLCPP_INFO(this->get_logger(), "graph search_radius: %.2f m", params_.graph.search_radius);
    }
}

void RoutingNode::initializeTopicHandlers() {
    const auto qos = static_cast<rmw_qos_reliability_policy_t>(
        this->declare_parameter<int>("qos", RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT));

    slots_.odom = this->create_subscription<nav_msgs::msg::Odometry>(
        "/ekf/odometry/filtered",
        rclcpp::QoS(1).reliability(qos),
        std::bind(&RoutingNode::onOdometry, this, _1));

    slots_.finish = this->create_subscription<geometry_msgs::msg::PointStamped>(
        "/clicked_point",
        rclcpp::QoS(1).reliability(qos),
        std::bind(&RoutingNode::onFinish, this, _1));

    signals_.mesh = this->create_publisher<truck_msgs::msg::NavigationMesh>(
        "/navigation/mesh", rclcpp::QoS(1).reliability(qos));

    signals_.route = this->create_publisher<truck_msgs::msg::NavigationRoute>(
        "/navigation/route", rclcpp::QoS(1).reliability(qos));

    timers_.mesh = this->create_wall_timer(1s, std::bind(&RoutingNode::makeMeshTick, this));
    timers_.route = this->create_wall_timer(250ms, std::bind(&RoutingNode::makeRouteTick, this));
}

void RoutingNode::onOdometry(const nav_msgs::msg::Odometry::SharedPtr msg) {
    state_.ego = geom::toVec2(*msg);
}

void RoutingNode::onFinish(const geometry_msgs::msg::PointStamped::SharedPtr msg) {
    state_.finish = geom::toVec2(*msg);

    if (state_.ego.has_value()) {
        updateRoute();
        RCLCPP_INFO(this->get_logger(), "Update route finish point.");
    }
}

void RoutingNode::updateRoute() {
    geom::Polyline polyline = cache_.findPath(state_.ego.value(), state_.finish.value());
    size_t polyline_points = polyline.size();

    geom::Polyline polyline_smoothed;

    if (polyline_points >= 2) {
        if (polyline_points == 2) {
            polyline_smoothed = geom::toLinearSpline(polyline, params_.route.spline_step);
        } else {
            polyline_smoothed = geom::toQuadraticSpline(polyline, params_.route.spline_step);
        }
    } else {
        RCLCPP_INFO(this->get_logger(), "Route not found.");
        return;
    }

    state_.route = Route(polyline_smoothed);
}

void RoutingNode::makeMeshTick() { publishMesh(); }

void RoutingNode::makeRouteTick() {
    if (!state_.route.has_value()) {
        return;
    }

    if (state_.route->distance(state_.ego.value()) > params_.route.max_ego_dist) {
        updateRoute();
        RCLCPP_INFO(this->get_logger(), "Update route start point.");
    }

    publishRoute();
}

void RoutingNode::publishMesh() const {
    truck_msgs::msg::NavigationMesh msg;

    for (const auto& node : cache_.graph.nodes) {
        msg.data.emplace_back(geom::msg::toPoint(node.point));
    }

    signals_.mesh->publish(msg);
}

void RoutingNode::publishRoute() const {
    truck_msgs::msg::NavigationRoute msg;

    msg.postfix_index = state_.route->postfixIndex(state_.ego.value(), params_.route.postfix_len);

    for (const geom::Vec2& point : state_.route->polyline) {
        msg.data.emplace_back(geom::msg::toPoint(point));
    }

    signals_.route->publish(msg);
}

}  // namespace truck::routing