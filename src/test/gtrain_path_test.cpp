#include <game/server/gamemodes/gtrain_path.h>

#include <gtest/gtest.h>

#include <queue>

namespace
{
	CGTrainPathGraph Grid(int Width, int Height, const std::vector<int> &vBlocked = {})
	{
		CGTrainPathGraph Graph;
		std::vector<bool> vPassable(Width * Height, true);
		for(int Index : vBlocked)
			vPassable[Index] = false;
		for(int y = 0; y < Height; ++y)
			for(int x = 0; x < Width; ++x)
				Graph.m_vNodes.push_back({vec2(x * 32 + 16, y * 32 + 16), vPassable[y * Width + x]});
		std::vector<std::pair<int, CGTrainPathGraph::CEdge>> vEdges;
		std::vector<int> vTileNodes(Width * Height, -1);
		for(int i = 0; i < Width * Height; ++i)
			if(vPassable[i])
				vTileNodes[i] = i;
		for(int i = 0; i < Width * Height; ++i)
			CGTrainPathGraph::AddTileEdges(i, Width, Height, vTileNodes, vEdges);
		Graph.InitEdges(vEdges);
		return Graph;
	}

}

TEST(GTrainPath, ExactPathDistanceAndObstacleRoute)
{
	const auto Graph = Grid(7, 5, {3, 10, 17, 24});
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Goal;
	Finder.Build(Goal, 0, 10, 123);
	ASSERT_TRUE(Goal.m_Ready);
	EXPECT_EQ(Goal.m_Distance, 10);
	ASSERT_EQ(Goal.m_vInitialRoute.size(), 11);
	std::vector<int> vRoute;
	ASSERT_TRUE(Finder.FindPath(Goal, 0, vRoute));
	EXPECT_EQ(vRoute, Goal.m_vInitialRoute);
	EXPECT_EQ(vRoute.front(), 0);
	EXPECT_EQ(vRoute.back(), Goal.m_Goal);
	for(size_t i = 1; i < vRoute.size(); ++i)
	{
		const int From = vRoute[i - 1], To = vRoute[i];
		bool Adjacent = false;
		for(int j = Graph.m_vOutOffsets[From]; j < Graph.m_vOutOffsets[From + 1]; ++j)
			Adjacent |= Graph.m_vOutEdges[j].m_Node == To;
		EXPECT_TRUE(Adjacent);
	}
}

TEST(GTrainPath, ShortMapFallsBackToFarthestEligibleGoal)
{
	auto Graph = Grid(8, 1);
	Graph.m_vNodes[7].m_Goal = false;
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Goal;
	Finder.Build(Goal, 0, 100, 1);
	ASSERT_TRUE(Goal.m_Ready);
	EXPECT_EQ(Goal.m_Goal, 6);
	EXPECT_EQ(Goal.m_Distance, 6);
	EXPECT_EQ(Goal.m_vInitialRoute.size(), 7);
}

TEST(GTrainPath, DirectedTeleportAndDisconnectedRegion)
{
	CGTrainPathGraph Graph;
	for(int i = 0; i < 5; ++i)
		Graph.m_vNodes.push_back({vec2(i * 320, 16), i == 3});
	Graph.InitEdges({{0, {1, 2}}, {1, {2, 0}}, {2, {3, 8}}});
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Goal;
	Finder.Build(Goal, 0, 3, 1);
	ASSERT_TRUE(Goal.m_Ready);
	EXPECT_EQ(Goal.m_Goal, 3);
	EXPECT_EQ(Goal.m_vInitialRoute, (std::vector<int>{0, 1, 2, 3}));
	EXPECT_TRUE(Graph.IsTeleportStep(1, 2));
	EXPECT_FALSE(Graph.IsTeleportStep(0, 1));
	std::vector<int> vRoute;
	EXPECT_TRUE(Finder.FindPath(Goal, 0, vRoute));
	EXPECT_EQ(vRoute, Goal.m_vInitialRoute);
	EXPECT_FALSE(Finder.FindPath(Goal, 4, vRoute));
	EXPECT_TRUE(vRoute.empty());
	CGTrainGoal Back;
	Finder.BuildField(Back, 0, 100);
	EXPECT_FALSE(Finder.FindPath(Back, 3, vRoute));
	EXPECT_TRUE(vRoute.empty());
}

TEST(GTrainPath, TeleportShortcutAwayFromTheGoal)
{
	auto Graph = Grid(20, 1);
	std::vector<std::pair<int, CGTrainPathGraph::CEdge>> vEdges;
	for(int i = 0; i < 19; ++i)
		vEdges.push_back({i, {i + 1, 2}});
	// The optimal path first moves away from the goal, then teleports.
	vEdges.push_back({10, {0, 0}});
	vEdges.push_back({0, {19, 0}});
	Graph.InitEdges(vEdges);
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Goal;
	Finder.BuildField(Goal, 19, 100);
	std::vector<int> vRoute;
	ASSERT_TRUE(Finder.FindPath(Goal, 10, vRoute));
	EXPECT_EQ(vRoute, (std::vector<int>{10, 0, 19}));
}

TEST(GTrainPath, SeparateGoalsAndRebuilding)
{
	const auto Graph = Grid(20, 1);
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal First, Second;
	Finder.Build(First, 0, 19, 1);
	const auto vOriginal = First.m_vInitialRoute;
	Finder.Build(Second, 19, 19, 2);
	EXPECT_EQ(First.m_vInitialRoute, vOriginal);
	EXPECT_EQ(First.m_Goal, 19);
	EXPECT_EQ(Second.m_Goal, 0);
	Finder.Build(First, 10, 3, 3);
	ASSERT_TRUE(First.m_Ready);
	EXPECT_EQ(First.m_Distance, 3);
	EXPECT_EQ(First.m_vInitialRoute.size(), 4);
}

TEST(GTrainPath, NoGoalAndInvalidStart)
{
	const auto Graph = Grid(1, 1);
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Goal;
	Finder.Build(Goal, 0, 100, 1);
	EXPECT_FALSE(Goal.m_Ready);
	Finder.Build(Goal, -1, 100, 1);
	EXPECT_FALSE(Goal.m_Ready);
	std::vector<int> vRoute;
	Finder.BuildField(Goal, 0, 100);
	EXPECT_FALSE(Finder.FindPath(Goal, -1, vRoute));
	EXPECT_FALSE(Finder.FindPath(Goal, 1, vRoute));
	EXPECT_TRUE(Finder.FindPath(Goal, 0, vRoute));
	EXPECT_EQ(vRoute, (std::vector<int>{0}));
	Finder.BuildField(Goal, 1, 100);
	EXPECT_FALSE(Finder.FindPath(Goal, 0, vRoute));
}

TEST(GTrainPath, FieldRoutesMatchBreadthFirstSearchAroundWalls)
{
	const auto Graph = Grid(11, 9, {3, 14, 25, 36, 47, 58, 69, 71, 82, 93});
	CGTrainPathfinder Finder(Graph);
	std::vector<CGTrainGoal> vGoals(Graph.m_vNodes.size());
	for(int Goal = 0; Goal < (int)Graph.m_vNodes.size(); ++Goal)
		Finder.BuildField(vGoals[Goal], Goal, 1000);
	for(int Start : {0, 10, 44, 98})
	{
		std::vector<int> vDistances(Graph.m_vNodes.size(), -1);
		std::queue<int> Queue;
		vDistances[Start] = 0;
		Queue.push(Start);
		while(!Queue.empty())
		{
			const int Node = Queue.front();
			Queue.pop();
			for(int i = Graph.m_vOutOffsets[Node]; i < Graph.m_vOutOffsets[Node + 1]; ++i)
			{
				const int Next = Graph.m_vOutEdges[i].m_Node;
				if(vDistances[Next] >= 0)
					continue;
				vDistances[Next] = vDistances[Node] + 1;
				Queue.push(Next);
			}
		}
		for(int Goal = 0; Goal < (int)Graph.m_vNodes.size(); ++Goal)
		{
			std::vector<int> vRoute;
			EXPECT_EQ(Finder.FindPath(vGoals[Goal], Start, vRoute), vDistances[Goal] >= 0);
			if(vDistances[Goal] >= 0)
			{
				EXPECT_EQ((int)vRoute.size() - 1, vDistances[Goal]);
				EXPECT_EQ(vRoute.back(), Goal);
				for(size_t i = 1; i < vRoute.size(); ++i)
				{
					bool Adjacent = false;
					for(int j = Graph.m_vOutOffsets[vRoute[i - 1]]; j < Graph.m_vOutOffsets[vRoute[i - 1] + 1]; ++j)
						Adjacent |= Graph.m_vOutEdges[j].m_Node == vRoute[i];
					EXPECT_TRUE(Adjacent);
				}
			}
		}
	}
}

TEST(GTrainPath, DiagonalStepsAndGoalDistance)
{
	const auto Graph = Grid(4, 4);
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Corner;
	Finder.BuildField(Corner, 15, 100);
	std::vector<int> vRoute;
	ASSERT_TRUE(Finder.FindPath(Corner, 0, vRoute));
	EXPECT_EQ(vRoute, (std::vector<int>{0, 5, 10, 15}));
	CGTrainGoal Goal;
	Finder.Build(Goal, 0, 3, 9);
	ASSERT_TRUE(Goal.m_Ready);
	EXPECT_EQ(Goal.m_Distance, 3);
	EXPECT_EQ(Goal.m_vInitialRoute.size(), 4);
	const vec2 Offset = Graph.m_vNodes[Goal.m_Goal].m_Pos - Graph.m_vNodes[0].m_Pos;
	EXPECT_FLOAT_EQ(std::max(std::abs(Offset.x), std::abs(Offset.y)), 3 * 32.0f);
}

TEST(GTrainPath, DiagonalsRequireBothSideTilesToBeSafe)
{
	for(const auto &vBlocked : {std::vector<int>{1}, std::vector<int>{2}, std::vector<int>{1, 2}})
	{
		const auto Graph = Grid(2, 2, vBlocked);
		for(int i = Graph.m_vOutOffsets[0]; i < Graph.m_vOutOffsets[1]; ++i)
			EXPECT_NE(Graph.m_vOutEdges[i].m_Node, 3);
		CGTrainPathfinder Finder(Graph);
		CGTrainGoal Goal;
		Finder.BuildField(Goal, 3, 100);
		std::vector<int> vRoute;
		if(vBlocked.size() == 1)
		{
			ASSERT_TRUE(Finder.FindPath(Goal, 0, vRoute));
			EXPECT_EQ(vRoute.size(), 3); // must take two cardinal steps
		}
		else
			EXPECT_FALSE(Finder.FindPath(Goal, 0, vRoute));
	}
}

TEST(GTrainPath, FieldDepthCoversDetoursBehindTheStart)
{
	const auto Graph = Grid(200, 1);
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Goal;
	Finder.BuildField(Goal, 0, 5);
	std::vector<int> vRoute;
	EXPECT_TRUE(Finder.FindPath(Goal, 5, vRoute));
	EXPECT_EQ(vRoute.size(), 6);
	EXPECT_FALSE(Finder.FindPath(Goal, 6, vRoute));
	EXPECT_TRUE(vRoute.empty());
	EXPECT_EQ(Goal.m_vField.size(), 6); // only the reached node range is stored

	// Built goals allow straying as far behind the start as the goal is ahead.
	Finder.Build(Goal, 100, 10, 1);
	ASSERT_TRUE(Goal.m_Ready);
	const int Behind = Goal.m_Goal < 100 ? 1 : -1;
	EXPECT_TRUE(Finder.FindPath(Goal, 100 + Behind * (10 + CGTrainPathfinder::FIELD_MARGIN), vRoute));
	EXPECT_EQ((int)vRoute.size() - 1, 2 * 10 + CGTrainPathfinder::FIELD_MARGIN);
	EXPECT_FALSE(Finder.FindPath(Goal, 100 + Behind * (11 + CGTrainPathfinder::FIELD_MARGIN), vRoute));
}

TEST(GTrainPath, EqualRoutesHeadStraightForTheGoal)
{
	const auto Graph = Grid(8, 4);
	CGTrainPathfinder Finder(Graph);
	CGTrainGoal Goal;
	Finder.BuildField(Goal, 2 * 8 + 7, 100);
	std::vector<int> vRoute;
	ASSERT_TRUE(Finder.FindPath(Goal, 0, vRoute));
	// Two diagonal steps reach the goal row, then the route continues straight.
	EXPECT_EQ(vRoute, (std::vector<int>{0, 9, 18, 19, 20, 21, 22, 23}));
}
