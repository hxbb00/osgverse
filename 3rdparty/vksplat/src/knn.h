#pragma once

#include <vector>
#include <queue>
#include <algorithm>
#include <memory>
#include <limits>
#include <Eigen/Core>


class NearestNeighbors3D {
private:
    struct KDNode {
        Eigen::Vector3f point;
        size_t index;  // Original index in the dataset
        int axis;      // Split axis (0=x, 1=y, 2=z)
        std::unique_ptr<KDNode> left;
        std::unique_ptr<KDNode> right;
        
        KDNode(const Eigen::Vector3f& p, size_t idx, int a) 
            : point(p), index(idx), axis(a), left(nullptr), right(nullptr) {}
    };
    
    struct NeighborResult {
        float distance;
        size_t index;
        Eigen::Vector3f point;
        
        bool operator>(const NeighborResult& other) const {
            return distance > other.distance;
        }
        bool operator<(const NeighborResult& other) const {
            return distance < other.distance;
        }
    };
    
    std::unique_ptr<KDNode> root;
    std::vector<Eigen::Vector3f> data_points;
    bool is_fitted;
    
    // Build KD-tree recursively
    std::unique_ptr<KDNode> build_tree(
        std::vector<std::pair<Eigen::Vector3f, size_t>>& points, 
        int depth = 0
    );
    
    // Search KD-tree for k nearest neighbors
    void search_tree(
        const KDNode* node,
        const Eigen::Vector3f& query_point,
        int k,
        std::priority_queue<NeighborResult, std::vector<NeighborResult>, 
                std::less<NeighborResult>>& neighbors
    ) const;
    
    // Search within radius
    void radius_search_tree(
        const KDNode* node,
        const Eigen::Vector3f& query_point,
        float radius,
        std::vector<NeighborResult>& neighbors
    ) const;

public:
    NearestNeighbors3D() : is_fitted(false) {}
    
    // Fit the model with training data (similar to sklearn.fit())
    void fit(const std::vector<Eigen::Vector3f>& X);
    
    // Find k nearest neighbors (similar to sklearn.kneighbors())
    std::pair<std::vector<std::vector<float>>, std::vector<std::vector<size_t>>> 
    kneighbors(
        const std::vector<Eigen::Vector3f>& X,
        int n_neighbors = 5,
        bool return_distance = true
    ) const;
    
    // Find neighbors within a given radius (similar to sklearn.radius_neighbors())
    std::pair<std::vector<std::vector<float>>, std::vector<std::vector<size_t>>>
    radius_neighbors(
        const std::vector<Eigen::Vector3f>& X,
        float radius,
        bool return_distance = true
    ) const;
    
    // Single point k-nearest neighbors query
    std::pair<std::vector<float>, std::vector<size_t>>
    kneighbors_single(const Eigen::Vector3f& query_point, int n_neighbors = 5, bool return_distance = true) const;
    
    // Single point radius neighbors query
    std::pair<std::vector<float>, std::vector<size_t>>
    radius_neighbors_single(const Eigen::Vector3f& query_point, float radius, bool return_distance = true) const;
    
    // Get the fitted data
    const std::vector<Eigen::Vector3f>& get_data() const;
    
    // Check if model is fitted
    bool is_fitted_model() const {
        return is_fitted;
    }
    
    // Get number of samples in fitted data
    size_t n_samples() const {
        return is_fitted ? data_points.size() : 0;
    }
};
