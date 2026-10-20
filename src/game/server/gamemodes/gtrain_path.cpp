#include "gtrain_path.h"

#include <algorithm>
#include <random>

void CGTrainPathGraph::AddTileEdges(int Index, int Width, int Height, const std::vector<int> &vTileNodes, const std::vector<uint8_t> &vStoppers, std::vector<std::pair<int, CEdge>> &vEdges)
{
	const int Node = vTileNodes[Index];
	if(Node < 0)
		return;
	const auto Blocked = [&](int From, int To, int Direction) {
		return !vStoppers.empty() && ((vStoppers[To] & Direction) || ((vStoppers[From] >> 4) & Direction));
	};
	const int x = Index % Width, y = Index / Width;
	for(int dy = -1; dy <= 1; ++dy)
		for(int dx = -1; dx <= 1; ++dx)
		{
			if((dx == 0 && dy == 0) || x + dx < 0 || x + dx >= Width || y + dy < 0 || y + dy >= Height)
				continue;
			const int DestinationIndex = (y + dy) * Width + x + dx;
			const int Destination = vTileNodes[DestinationIndex];
			if(Destination < 0)
				continue;
			const int Horizontal = dx < 0 ? DIRECTION_LEFT : (dx > 0 ? DIRECTION_RIGHT : 0);
			const int Vertical = dy < 0 ? DIRECTION_UP : (dy > 0 ? DIRECTION_DOWN : 0);
			if(dx != 0 && dy != 0)
			{
				// Both tiles beside a diagonal must be safe. Merely checking its
				// destination would allow squeezing past solid or freeze corners.
				// Stoppers must also allow both ways around the corner.
				const int SideX = y * Width + x + dx, SideY = (y + dy) * Width + x;
				if(vTileNodes[SideX] < 0 || vTileNodes[SideY] < 0 ||
					Blocked(Index, SideX, Horizontal) || Blocked(SideX, DestinationIndex, Vertical) ||
					Blocked(Index, SideY, Vertical) || Blocked(SideY, DestinationIndex, Horizontal))
					continue;
			}
			else if(Blocked(Index, DestinationIndex, Horizontal | Vertical))
				continue;
			vEdges.push_back({Node, {Destination, (uint8_t)(Horizontal | Vertical)}});
		}
}

void CGTrainPathGraph::InitEdges(const std::vector<std::pair<int, CEdge>> &vEdges)
{
	m_vOutOffsets.assign(m_vNodes.size() + 1, 0);
	m_vInOffsets.assign(m_vNodes.size() + 1, 0);
	for(const auto &Edge : vEdges)
	{
		++m_vOutOffsets[Edge.first + 1];
		++m_vInOffsets[Edge.second.m_Node + 1];
	}
	for(size_t i = 1; i < m_vOutOffsets.size(); ++i)
	{
		m_vOutOffsets[i] += m_vOutOffsets[i - 1];
		m_vInOffsets[i] += m_vInOffsets[i - 1];
	}
	m_vOutEdges.resize(vEdges.size());
	m_vInNodes.resize(vEdges.size());
	auto vOutCursor = m_vOutOffsets;
	auto vInCursor = m_vInOffsets;
	for(const auto &Edge : vEdges)
	{
		m_vOutEdges[vOutCursor[Edge.first]++] = Edge.second;
		m_vInNodes[vInCursor[Edge.second.m_Node]++] = Edge.first;
	}
}

bool CGTrainPathGraph::IsTeleportStep(int From, int To) const
{
	for(int i = m_vOutOffsets[From]; i < m_vOutOffsets[From + 1]; ++i)
		if(m_vOutEdges[i].m_Node == To)
			return m_vOutEdges[i].m_Direction == 0;
	return false;
}

CGTrainPathfinder::CGTrainPathfinder(const CGTrainPathGraph &Graph) :
	m_Graph(Graph)
{
}

void CGTrainPathfinder::BeginSearch()
{
	m_vStates.resize(m_Graph.m_vNodes.size());
	if(++m_Stamp == 0)
	{
		for(auto &State : m_vStates)
			State.m_Stamp = 0;
		++m_Stamp;
	}
}

void CGTrainPathfinder::BuildField(CGTrainGoal &Goal, int Node, int Depth)
{
	Goal.m_Goal = Node;
	Goal.m_FieldBegin = 0;
	Goal.m_vField.clear();
	if(Node < 0 || Node >= (int)m_Graph.m_vNodes.size())
		return;
	Depth = std::clamp(Depth, 0, (int)MAX_FIELD_DEPTH);
	BeginSearch();
	m_vQueue.clear();
	m_vQueue.push_back(Node);
	m_vStates[Node] = {m_Stamp, 0};
	int Begin = Node, End = Node;
	// Reverse BFS over incoming edges yields the distance towards the goal.
	for(size_t Head = 0; Head < m_vQueue.size(); ++Head)
	{
		const int Current = m_vQueue[Head];
		const int Distance = m_vStates[Current].m_Cost;
		if(Distance >= Depth)
			break; // the queue is ordered by distance
		for(int i = m_Graph.m_vInOffsets[Current]; i < m_Graph.m_vInOffsets[Current + 1]; ++i)
		{
			const int Previous = m_Graph.m_vInNodes[i];
			if(m_vStates[Previous].m_Stamp == m_Stamp)
				continue;
			m_vStates[Previous] = {m_Stamp, Distance + 1};
			m_vQueue.push_back(Previous);
			Begin = std::min(Begin, Previous);
			End = std::max(End, Previous);
		}
	}
	// Node indices follow the map rows, so a field around one goal spans a
	// small index range even on large maps, unless teleports leave it.
	Goal.m_FieldBegin = Begin;
	Goal.m_vField.assign(End - Begin + 1, CGTrainGoal::NO_DISTANCE);
	for(const int Visited : m_vQueue)
		Goal.m_vField[Visited - Begin] = m_vStates[Visited].m_Cost;
}

bool CGTrainPathfinder::FindPath(const CGTrainGoal &Goal, int Start, std::vector<int> &vRoute) const
{
	vRoute.clear();
	if(Start < 0 || Start >= (int)m_Graph.m_vNodes.size())
		return false;
	int Distance = Goal.FieldDistance(Start);
	if(Distance < 0)
		return false;
	const vec2 Target = m_Graph.m_vNodes[Goal.m_Goal].m_Pos;
	vRoute.reserve(Distance + 1);
	vRoute.push_back(Start);
	// Every node in the field has a neighbor one step closer. Among equally
	// short steps, the one nearest to the goal keeps open routes straight.
	for(int Node = Start; Distance > 0; --Distance)
	{
		int Next = -1;
		float NextDistance = 0.0f;
		for(int i = m_Graph.m_vOutOffsets[Node]; i < m_Graph.m_vOutOffsets[Node + 1]; ++i)
		{
			const int Candidate = m_Graph.m_vOutEdges[i].m_Node;
			if(Goal.FieldDistance(Candidate) != Distance - 1)
				continue;
			const vec2 Offset = m_Graph.m_vNodes[Candidate].m_Pos - Target;
			const float CandidateDistance = dot(Offset, Offset);
			if(Next < 0 || CandidateDistance < NextDistance)
			{
				Next = Candidate;
				NextDistance = CandidateDistance;
			}
		}
		Node = Next;
		vRoute.push_back(Node);
	}
	return true;
}

void CGTrainPathfinder::FindReachable(const std::vector<int> &vSources, int Depth, std::vector<int> &vReachable)
{
	vReachable.clear();
	BeginSearch();
	for(const int Source : vSources)
	{
		if(Source < 0 || Source >= (int)m_Graph.m_vNodes.size() || m_vStates[Source].m_Stamp == m_Stamp)
			continue;
		m_vStates[Source] = {m_Stamp, 0};
		vReachable.push_back(Source);
	}
	// The result doubles as the BFS queue, which is ordered by distance.
	for(size_t Head = 0; Head < vReachable.size(); ++Head)
	{
		const int Node = vReachable[Head];
		const int Distance = m_vStates[Node].m_Cost;
		if(Distance >= Depth)
			break;
		for(int i = m_Graph.m_vOutOffsets[Node]; i < m_Graph.m_vOutOffsets[Node + 1]; ++i)
		{
			const int Next = m_Graph.m_vOutEdges[i].m_Node;
			if(m_vStates[Next].m_Stamp == m_Stamp)
				continue;
			m_vStates[Next] = {m_Stamp, Distance + 1};
			vReachable.push_back(Next);
		}
	}
}

void CGTrainPathfinder::Build(CGTrainGoal &Goal, int Start, int Distance, unsigned Seed)
{
	Goal.m_Ready = false;
	Goal.m_Goal = -1;
	Goal.m_Distance = -1;
	Goal.m_vInitialRoute.clear();
	Goal.m_vField.clear();
	if(Start < 0 || Start >= (int)m_Graph.m_vNodes.size())
		return;
	BeginSearch();
	m_vQueue.clear();
	m_vQueue.push_back(Start);
	m_vStates[Start] = {m_Stamp, 0};
	std::mt19937 Random(Seed);
	int GoalNode = -1;
	int NumCandidates = 0;
	// BFS chooses a goal by actual path tile distance, not raw displacement.
	for(size_t Head = 0; Head < m_vQueue.size(); ++Head)
	{
		const int Node = m_vQueue[Head];
		const int Depth = m_vStates[Node].m_Cost;
		if(Depth > 0 && m_Graph.m_vNodes[Node].m_Goal)
		{
			if(Depth > Goal.m_Distance)
			{
				Goal.m_Distance = Depth;
				NumCandidates = 0;
			}
			if(std::uniform_int_distribution<int>(1, ++NumCandidates)(Random) == 1)
				GoalNode = Node;
		}
		if(Depth >= Distance)
			continue;
		for(int i = m_Graph.m_vOutOffsets[Node]; i < m_Graph.m_vOutOffsets[Node + 1]; ++i)
		{
			const int Next = m_Graph.m_vOutEdges[i].m_Node;
			if(m_vStates[Next].m_Stamp == m_Stamp)
				continue;
			m_vStates[Next] = {m_Stamp, Depth + 1};
			m_vQueue.push_back(Next);
		}
	}
	if(GoalNode < 0)
		return;
	BuildField(Goal, GoalNode, 2 * Goal.m_Distance + FIELD_MARGIN);
	FindPath(Goal, Start, Goal.m_vInitialRoute);
	Goal.m_Ready = true;
}
