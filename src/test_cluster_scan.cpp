/*
 * test_cluster_scan.cpp
 */

#include "test_base_class.h"

#include "cluster_scan.h"

using namespace std;

class ClusterScanTest : public TestBaseClass {
   protected:
    void SetUp(void) { TestBaseClass::SetUp(); }

    void TearDown(void) { TestBaseClass::TearDown(); }
};

TEST_F(ClusterScanTest, simple_close_frequencies) {
    vector<ClusterScanFrequency> frequencies = {
        {120000000, 0},
        {120050000, 1},
        {120100000, 2},
    };

    ClusterScanPlan plan = build_cluster_scan_plan(frequencies, 2400000, 150000, 0, true);

    ASSERT_EQ(plan.clusters.size(), 1u);
    EXPECT_EQ(plan.usable_bandwidth_hz, 2100000);
    EXPECT_EQ(plan.clusters[0].center_frequency, 120050000);
    EXPECT_EQ(plan.clusters[0].channel_indices.size(), 3u);
}

TEST_F(ClusterScanTest, frequencies_exactly_at_bandwidth_edge) {
    vector<ClusterScanFrequency> frequencies = {
        {120000000, 0},
        {122100000, 1},
    };

    ClusterScanPlan plan = build_cluster_scan_plan(frequencies, 2400000, 150000, 0, true);

    ASSERT_EQ(plan.clusters.size(), 1u);
    EXPECT_EQ(plan.clusters[0].min_frequency, 120000000);
    EXPECT_EQ(plan.clusters[0].max_frequency, 122100000);
    EXPECT_EQ(plan.clusters[0].center_frequency, 121050000);
}

TEST_F(ClusterScanTest, frequencies_requiring_multiple_clusters) {
    vector<ClusterScanFrequency> frequencies = {
        {120000000, 0},
        {120100000, 1},
        {123000000, 2},
    };

    ClusterScanPlan plan = build_cluster_scan_plan(frequencies, 2400000, 150000, 0, true);

    ASSERT_EQ(plan.clusters.size(), 2u);
    EXPECT_EQ(plan.clusters[0].channel_indices.size(), 2u);
    EXPECT_EQ(plan.clusters[1].channel_indices.size(), 1u);
    EXPECT_EQ(plan.clusters[1].center_frequency, 123000000);
}

TEST_F(ClusterScanTest, edge_guard_behavior) {
    vector<ClusterScanFrequency> frequencies = {
        {120000000, 0},
        {121900000, 1},
    };

    ClusterScanPlan conservative = build_cluster_scan_plan(frequencies, 2400000, 300000, 0, true);
    ClusterScanPlan less_guard = build_cluster_scan_plan(frequencies, 2400000, 150000, 0, true);

    EXPECT_EQ(conservative.usable_bandwidth_hz, 1800000);
    EXPECT_EQ(conservative.clusters.size(), 2u);
    EXPECT_EQ(less_guard.usable_bandwidth_hz, 2100000);
    EXPECT_EQ(less_guard.clusters.size(), 1u);
}

TEST_F(ClusterScanTest, multiple_device_assignment) {
    vector<vector<int> > assignments = assign_clusters_round_robin(5, 2);

    ASSERT_EQ(assignments.size(), 2u);
    ASSERT_EQ(assignments[0].size(), 3u);
    ASSERT_EQ(assignments[1].size(), 2u);
    EXPECT_EQ(assignments[0][0], 0);
    EXPECT_EQ(assignments[0][1], 2);
    EXPECT_EQ(assignments[0][2], 4);
    EXPECT_EQ(assignments[1][0], 1);
    EXPECT_EQ(assignments[1][1], 3);
}

TEST_F(ClusterScanTest, max_cluster_width_limits_cluster) {
    vector<ClusterScanFrequency> frequencies = {
        {120000000, 0},
        {120600000, 1},
    };

    ClusterScanPlan plan = build_cluster_scan_plan(frequencies, 2400000, 150000, 500000, true);

    EXPECT_EQ(plan.usable_bandwidth_hz, 500000);
    EXPECT_EQ(plan.clusters.size(), 2u);
}

TEST_F(ClusterScanTest, prefer_more_channels_false_uses_single_channel_clusters) {
    vector<ClusterScanFrequency> frequencies = {
        {120000000, 0},
        {120050000, 1},
        {120100000, 2},
    };

    ClusterScanPlan plan = build_cluster_scan_plan(frequencies, 2400000, 150000, 0, false);

    ASSERT_EQ(plan.clusters.size(), 3u);
    EXPECT_EQ(plan.clusters[0].channel_indices.size(), 1u);
    EXPECT_EQ(plan.clusters[1].channel_indices.size(), 1u);
    EXPECT_EQ(plan.clusters[2].channel_indices.size(), 1u);
}
