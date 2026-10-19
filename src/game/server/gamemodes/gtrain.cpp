#include "gtrain.h"

#include <base/fs.h>
#include <base/log.h>
#include <base/secure.h>

#include <engine/shared/config.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <generated/server_data.h>

#include <game/collision.h>
#include <game/mapitems.h>
#include <game/server/entities/character.h>
#include <game/server/gamecontext.h>
#include <game/server/player.h>

#include <algorithm>
#include <iterator>

#define GAME_TYPE_NAME "GTrain"
#define TEST_TYPE_NAME "TestGTrain"

CGameControllerGTrain::CGameControllerGTrain(class CGameContext *pGameServer) :
	CGameControllerDDNet(pGameServer),
	m_Pathfinder(m_PathGraph)
{
	m_pGameType = g_Config.m_SvTestingCommands ? TEST_TYPE_NAME : GAME_TYPE_NAME;
	m_GameFlags = 0; // Fight wins are numeric scores, including on 0.7 clients.

	std::fill(std::begin(m_aPendingPlace), std::end(m_aPendingPlace), false);
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		m_aFightGroup[ClientId] = ClientId;
	m_FlagSnapId = Server()->SnapNewId();
	for(auto &SnapId : m_aDirectionSnapIds)
		SnapId = Server()->SnapNewId();
	for(auto &SnapId : m_aDoorSnapIds)
		SnapId = Server()->SnapNewId();

	// Hover doors need door collision, which only exists with a switch layer
	// (see AddSwitchLayer), and a switch number the map leaves free. It stays
	// off for every team except while a door holds one of its tees.
	CCollision *pCollision = GameServer()->Collision();
	if(pCollision->SwitchLayer() && pCollision->m_HighestSwitchNumber < 255)
	{
		m_DoorSwitch = pCollision->m_HighestSwitchNumber + 1;
		pCollision->m_HighestSwitchNumber = m_DoorSwitch;
		GameServer()->m_World.m_Core.InitSwitchers(m_DoorSwitch);
		SSwitchers &Switcher = GameServer()->Switchers()[m_DoorSwitch];
		Switcher.m_Initial = false;
		std::fill(std::begin(Switcher.m_aStatus), std::end(Switcher.m_aStatus), false);
	}

	FindTrainPositions();
	Teams().SetScoreboardTeams(m_aScoreboardTeams);
}

CGameControllerGTrain::~CGameControllerGTrain()
{
	if(m_FlagSnapId)
		Server()->SnapFreeId(*m_FlagSnapId);
	for(auto SnapId : m_aDirectionSnapIds)
		if(SnapId)
			Server()->SnapFreeId(*SnapId);
	for(auto SnapId : m_aDoorSnapIds)
		if(SnapId)
			Server()->SnapFreeId(*SnapId);
}

bool CGameControllerGTrain::AddSwitchLayer(IStorage *pStorage, const char *pPath, char *pNewPath, int NewPathSize)
{
	// Door collision only exists in maps with a switch layer, on the server
	// and on clients. Maps without one are served with an empty one added in
	// a group of its own, which clients download like any changed map.
	pNewPath[0] = '\0';
	CDataFileReader Reader;
	if(!Reader.Open(pStorage, pPath, IStorage::TYPE_ALL))
		return false;
	int GroupsStart, NumGroups, LayersStart, NumLayers;
	Reader.GetType(MAPITEMTYPE_GROUP, &GroupsStart, &NumGroups);
	Reader.GetType(MAPITEMTYPE_LAYER, &LayersStart, &NumLayers);
	int Width = 0, Height = 0;
	for(int i = 0; i < NumLayers; ++i)
	{
		const auto *pLayer = static_cast<const CMapItemLayerTilemap_v2 *>(Reader.GetItem(LayersStart + i));
		if(Reader.GetItemSize(LayersStart + i) < (int)sizeof(CMapItemLayerTilemap_v2) || pLayer->m_Layer.m_Type != LAYERTYPE_TILES)
			continue;
		if(pLayer->m_Flags & TILESLAYERFLAG_SWITCH)
			return true;
		if(pLayer->m_Flags & TILESLAYERFLAG_GAME)
		{
			Width = pLayer->m_Width;
			Height = pLayer->m_Height;
		}
	}
	if(Width <= 0 || Height <= 0)
		return false;

	CDataFileWriter Writer;
	for(int i = 0; i < Reader.NumItems(); ++i)
	{
		int Type, Id;
		void *pData = Reader.GetItem(i, &Type, &Id);
		Writer.AddItem(Type, Id, Reader.GetItemSize(i), pData);
	}
	for(int i = 0; i < Reader.NumData(); ++i)
	{
		Writer.AddData(Reader.GetDataSize(i), Reader.GetData(i));
		Reader.UnloadData(i);
	}
	Reader.Close();

	CMapItemLayerTilemap Layer = {};
	Layer.m_Layer.m_Type = LAYERTYPE_TILES;
	Layer.m_Version = 3;
	Layer.m_Width = Width;
	Layer.m_Height = Height;
	Layer.m_Flags = TILESLAYERFLAG_SWITCH;
	Layer.m_Color = {255, 255, 255, 255};
	Layer.m_ColorEnv = -1;
	Layer.m_Image = -1;
	Layer.m_Tele = Layer.m_Speedup = Layer.m_Front = Layer.m_Tune = -1;
	const std::vector<CTile> vTiles((size_t)Width * Height, CTile{});
	const std::vector<CSwitchTile> vSwitchTiles((size_t)Width * Height, CSwitchTile{});
	Layer.m_Data = Writer.AddData(vTiles.size() * sizeof(CTile), vTiles.data());
	Layer.m_Switch = Writer.AddData(vSwitchTiles.size() * sizeof(CSwitchTile), vSwitchTiles.data());
	StrToInts(Layer.m_aName, std::size(Layer.m_aName), "Switch");
	Writer.AddItem(MAPITEMTYPE_LAYER, NumLayers, sizeof(Layer), &Layer);

	CMapItemGroup Group = {};
	Group.m_Version = 3;
	Group.m_ParallaxX = 100;
	Group.m_ParallaxY = 100;
	Group.m_StartLayer = NumLayers;
	Group.m_NumLayers = 1;
	StrToInts(Group.m_aName, std::size(Group.m_aName), "GTrain");
	Writer.AddItem(MAPITEMTYPE_GROUP, NumGroups, sizeof(Group), &Group);

	// The save path may lack the map's folder, its root always exists.
	char aTemp[IO_MAX_PATH_LENGTH];
	if(!Writer.Open(pStorage, IStorage::FormatTmpPath(aTemp, sizeof(aTemp), fs_filename(pPath))))
		return false;
	Writer.Finish();
	str_copy(pNewPath, aTemp, NewPathSize);
	return true;
}

void CGameControllerGTrain::FindTrainPositions()
{
	CCollision *pCollision = GameServer()->Collision();
	const int Width = pCollision->GetWidth();
	const int Height = pCollision->GetHeight();

	const auto HasTile = [&](int Index, int Tile) {
		return pCollision->GetTileIndex(Index) == Tile || pCollision->GetFrontTileIndex(Index) == Tile;
	};
	const auto IsBlocked = [&](int Index) {
		const int Tile = pCollision->GetTileIndex(Index);
		return Tile == TILE_SOLID || Tile == TILE_NOHOOK;
	};
	const auto IsCheckTeleport = [&](int Index) {
		return pCollision->IsCheckTeleport(Index) || pCollision->IsCheckEvilTeleport(Index);
	};
	const auto ForEachNeighbor = [&](int Index, auto &&Func) {
		const int x = Index % Width;
		const int y = Index / Width;
		if(x > 0)
			Func(Index - 1);
		if(x < Width - 1)
			Func(Index + 1);
		if(y > 0)
			Func(Index - Width);
		if(y < Height - 1)
			Func(Index + Width);
	};

	// flood-fill every side of the start line, the start and the finish
	// line are not crossed, so only the sides reaching the finish are kept
	std::vector<int> vSideOf(Width * Height, 0);
	std::vector<bool> vWalked(Width * Height, false); // reached from a start line neighbor without teleporting
	std::vector<bool> vKept(Width * Height, false);
	std::vector<bool> vDiscarded(Width * Height, false);
	std::vector<int> vSide;
	std::vector<int> vTeleported;
	std::vector<int> vTeleCheckpoints;
	int NumSides = 0;
	bool FoundStart = false;
	for(int StartIndex = 0; StartIndex < Width * Height; StartIndex++)
	{
		if(!HasTile(StartIndex, TILE_START))
			continue;
		FoundStart = true;

		ForEachNeighbor(StartIndex, [&](int SeedIndex) {
			if(vWalked[SeedIndex] || IsBlocked(SeedIndex) || HasTile(SeedIndex, TILE_START) || HasTile(SeedIndex, TILE_FINISH))
				return;

			const int Side = ++NumSides;
			bool HitsFinish = false;
			bool HitsCheckTeleport = false;
			const auto Reach = [&](int Index, std::vector<int> &vQueue) {
				if(HasTile(Index, TILE_FINISH))
					HitsFinish = true;
				else if(vSideOf[Index] != Side && !IsBlocked(Index) && !HasTile(Index, TILE_START))
				{
					vSideOf[Index] = Side;
					vQueue.push_back(Index);
				}
			};
			const auto TeleportTo = [&](const std::vector<vec2> &vTeleOuts) {
				for(const vec2 &TeleOut : vTeleOuts)
					Reach(pCollision->GetPureMapIndex(TeleOut), vTeleported);
			};
			const auto TeleportToCheckpoint = [&](int TeleCheckpoint) {
				// same order as the character, fall back to previous checkpoints
				for(int k = TeleCheckpoint - 1; k >= 0; k--)
				{
					if(!pCollision->TeleCheckOuts(k).empty())
					{
						TeleportTo(pCollision->TeleCheckOuts(k));
						return;
					}
				}
			};

			vSide.clear();
			vTeleported.clear();
			vTeleCheckpoints.clear();
			Reach(SeedIndex, vSide);
			size_t NumDone = 0;
			bool Walking = true;
			while(true)
			{
				for(; NumDone < vSide.size(); NumDone++)
				{
					const int Index = vSide[NumDone];
					if(Walking)
						vWalked[Index] = true;

					// teleporters can't be passed, the fill continues at their destinations
					const int Teleport = pCollision->IsTeleport(Index) ? pCollision->IsTeleport(Index) : pCollision->IsEvilTeleport(Index);
					if(Teleport)
					{
						TeleportTo(pCollision->TeleOuts(Teleport - 1));
						continue;
					}
					if(IsCheckTeleport(Index))
					{
						if(!HitsCheckTeleport)
						{
							HitsCheckTeleport = true;
							for(const int TeleCheckpoint : vTeleCheckpoints)
								TeleportToCheckpoint(TeleCheckpoint);
						}
						continue;
					}
					const int TeleCheckpoint = pCollision->IsTeleCheckpoint(Index);
					if(TeleCheckpoint && std::find(vTeleCheckpoints.begin(), vTeleCheckpoints.end(), TeleCheckpoint) == vTeleCheckpoints.end())
					{
						vTeleCheckpoints.push_back(TeleCheckpoint);
						if(HitsCheckTeleport)
							TeleportToCheckpoint(TeleCheckpoint);
					}

					ForEachNeighbor(Index, [&](int Neighbor) { Reach(Neighbor, vSide); });
				}
				if(vTeleported.empty())
					break;
				Walking = false;
				vSide.insert(vSide.end(), vTeleported.begin(), vTeleported.end());
				vTeleported.clear();
			}

			for(const int Index : vSide)
			{
				if(HitsFinish)
					vKept[Index] = true;
				else
					vDiscarded[Index] = true;
			}
		});
	}

	// positions must have freeze straight below, before any solid tile
	const auto HasFreezeBelow = [&](int Index) {
		for(int i = 1; i <= MAX_FREEZE_DISTANCE && Index + i * Width < Width * Height; i++)
		{
			const int Below = Index + i * Width;
			if(IsBlocked(Below))
				return false;
			if(HasTile(Below, TILE_FREEZE) || HasTile(Below, TILE_DFREEZE))
				return true;
		}
		return false;
	};

	for(int Index = 0; Index < Width * Height; Index++)
	{
		// sides not reaching the finish stay discarded, even if a teleporter leads back to them
		if(!vKept[Index] || vDiscarded[Index])
			continue;
		// death tiles and teleporters are discarded as well, to not kill or move the player instantly
		if(HasTile(Index, TILE_FREEZE) || HasTile(Index, TILE_DFREEZE) || HasTile(Index, TILE_DEATH))
			continue;
		if(pCollision->IsTeleport(Index) || pCollision->IsEvilTeleport(Index) || IsCheckTeleport(Index))
			continue;
		if(!HasFreezeBelow(Index))
			continue;
		m_vTrainPositions.emplace_back((Index % Width) * 32.0f + 16.0f, (Index / Width) * 32.0f + 16.0f);
	}

	if(m_vTrainPositions.empty())
		log_warn("gtrain", "no tiles found between start and finish line%s, training is disabled on this map", FoundStart ? "" : " (map has no start line)");
	else
		log_info("gtrain", "found %d tiles between start and finish line", (int)m_vTrainPositions.size());

	// Spawn eligibility and path eligibility differ: routes may pass safe tiles
	// without freeze below, but never route through freeze, death or race lines.
	m_vTileNodes.assign(Width * Height, -1);
	for(int Index = 0; Index < Width * Height; ++Index)
	{
		if(!vKept[Index] || vDiscarded[Index] || IsBlocked(Index) ||
			HasTile(Index, TILE_FREEZE) || HasTile(Index, TILE_DFREEZE) || HasTile(Index, TILE_DEATH) ||
			HasTile(Index, TILE_START) || HasTile(Index, TILE_FINISH) || IsCheckTeleport(Index))
			continue;
		// Checkpoint teleports depend on character history, so they cannot be
		// represented as unconditional edges in a shared static graph.
		m_vTileNodes[Index] = (int)m_PathGraph.m_vNodes.size();
		m_PathGraph.m_vNodes.push_back({vec2((Index % Width) * 32.0f + 16.0f, (Index / Width) * 32.0f + 16.0f), false});
	}
	for(vec2 Pos : m_vTrainPositions)
		m_PathGraph.m_vNodes[m_vTileNodes[pCollision->GetPureMapIndex(Pos)]].m_Goal = true;

	// Paths cannot pass stoppers against their direction. One-way stoppers
	// also block moving off them, like CCollision::GetMoveRestrictions.
	static_assert((int)CGTrainPathGraph::DIRECTION_LEFT == CANTMOVE_LEFT && (int)CGTrainPathGraph::DIRECTION_RIGHT == CANTMOVE_RIGHT &&
		      (int)CGTrainPathGraph::DIRECTION_UP == CANTMOVE_UP && (int)CGTrainPathGraph::DIRECTION_DOWN == CANTMOVE_DOWN);
	std::vector<uint8_t> vStoppers(Width * Height);
	for(int Index = 0; Index < Width * Height; ++Index)
		vStoppers[Index] = pCollision->GetTileMoveRestrictions(Index, false) | pCollision->GetTileMoveRestrictions(Index, true) << 4;
	std::vector<std::pair<int, CGTrainPathGraph::CEdge>> vEdges;
	for(int Index = 0; Index < Width * Height; ++Index)
	{
		const int Node = m_vTileNodes[Index];
		if(Node < 0)
			continue;
		const int Teleport = pCollision->IsTeleport(Index) ? pCollision->IsTeleport(Index) : pCollision->IsEvilTeleport(Index);
		if(Teleport)
		{
			for(vec2 Out : pCollision->TeleOuts(Teleport - 1))
			{
				const int Destination = m_vTileNodes[pCollision->GetPureMapIndex(Out)];
				if(Destination >= 0)
					vEdges.push_back({Node, {Destination, 0}});
			}
			continue;
		}
		CGTrainPathGraph::AddTileEdges(Index, Width, Height, m_vTileNodes, vStoppers, vEdges);
	}
	m_PathGraph.InitEdges(vEdges);
}

void CGameControllerGTrain::PlaceRandomly(CCharacter *pChr)
{
	const int ClientId = pChr->GetPlayer()->GetCid();
	m_aPendingPlace[ClientId] = false;
	if(m_vTrainPositions.empty())
		return;

	const vec2 Pos = m_vTrainPositions[secure_rand_below(m_vTrainPositions.size())];
	CAttempt &Attempt = m_aAttempts[ClientId];
	if(Attempt.m_pGoal.use_count() != 1)
		Attempt.m_pGoal = std::make_shared<CGTrainGoal>();
	const int Start = PathNode(Pos);
	const int Group = m_aFightGroup[ClientId];
	const int TargetDistance = GoalDistance(Group);
	if(m_aSettings[Group].m_Freeplay)
		*Attempt.m_pGoal = CGTrainGoal();
	else
		m_Pathfinder.Build(*Attempt.m_pGoal, Start, TargetDistance, secure_rand_below(1 << 30));
	PlaceForAttempt(pChr, Attempt.m_pGoal, Start, TargetDistance, false);
}

void CGameControllerGTrain::PlaceForAttempt(CCharacter *pChr, const std::shared_ptr<CGTrainGoal> &pGoal, int Start, int TargetDistance, bool Retry)
{
	const int ClientId = pChr->GetPlayer()->GetCid();
	const CSettings &Settings = m_aSettings[m_aFightGroup[ClientId]];
	m_aPendingPlace[ClientId] = false;
	CAttempt &Attempt = m_aAttempts[ClientId];
	// Goals are rebuilt in place, so a matching goal and start is not enough:
	// only retries count as fails.
	Attempt.m_Fails = Retry && Attempt.m_pGoal == pGoal && Attempt.m_Start == Start ? Attempt.m_Fails + 1 : 0;
	Attempt.m_pGoal = pGoal;
	Attempt.m_Start = Start;
	Attempt.m_TargetDistance = TargetDistance;
	Attempt.m_StartTick = -1;
	if(Settings.m_Freeplay)
		Attempt.m_vRoute.clear();
	else
		Attempt.m_vRoute = pGoal->m_vInitialRoute;
	Attempt.m_RouteIndex = 0;
	Attempt.m_RouteNode = Start;
	const vec2 Pos = m_PathGraph.m_vNodes[Start].m_Pos;
	pChr->SetDeepFrozen(false);
	pChr->SetLiveFrozen(false);
	pChr->Unfreeze();
	pChr->ResetHook();
	pChr->ResetJumps();
	if(Settings.m_NoDoubleJump)
		pChr->UseUpAirJumps();
	pChr->ResetVelocity();
	pChr->SetPosition(Pos);
	pChr->m_Pos = Pos;
	pChr->m_PrevPos = Pos;
	// the player didn't pass the start line, so finishing must not count
	pChr->m_DDRaceState = ERaceState::CHEATED;

	// Hover in place until the freeze is over. A tuning change only applies
	// on clients once it arrives, after their prediction passed the release.
	// A laser door below the tee instead holds it under normal gravity, and
	// its timed switch opens it on the last frozen tick: clients predict both
	// from snapshots, so the release looks the same at any ping.
	pChr->ForceFreeze(HOVER_TICKS);
	Attempt.m_DoorIndex = HoverDoorIndex(Start, pChr);
	if(Attempt.m_DoorIndex >= 0)
	{
		const vec2 DoorPos = GameServer()->Collision()->GetPos(Attempt.m_DoorIndex);
		GameServer()->Collision()->SetDoorCollisionAt(DoorPos.x, DoorPos.y, TILE_STOPA, 0, m_DoorSwitch);
		SSwitchers &Switcher = GameServer()->Switchers()[m_DoorSwitch];
		Switcher.m_aStatus[pChr->Team()] = true;
		Switcher.m_aEndTick[pChr->Team()] = Server()->Tick() + HOVER_TICKS - 1;
		Switcher.m_aType[pChr->Team()] = TILE_SWITCHTIMEDOPEN;
	}
	pChr->SetZeroGravity(Attempt.m_DoorIndex < 0);
}

int CGameControllerGTrain::HoverDoorIndex(int Start, CCharacter *pChr) const
{
	if(m_DoorSwitch < 0 || !g_Config.m_SvGtrainHoverDoor || pChr->Team() == TEAM_SUPER)
		return -1;
	CCollision *pCollision = GameServer()->Collision();
	const vec2 Below = m_PathGraph.m_vNodes[Start].m_Pos + vec2(0, 32);
	if(Below.y >= pCollision->GetHeight() * 32)
		return -1;
	// Clients only lay doors down from tiles without solid, death or laser
	// blockers. Doors of the map itself are kept.
	const int Index = pCollision->GetPureMapIndex(Below);
	CDoorTile Door;
	pCollision->GetDoorTile(Index, &Door);
	if(pCollision->GetTile(Below.x, Below.y) || pCollision->GetFrontTile(Below.x, Below.y) || (Door.m_Index && Door.m_Number != m_DoorSwitch))
		return -1;
	return Index;
}

void CGameControllerGTrain::DelayHoverDoor(CCharacter *pChr)
{
	// A paused tee stays frozen, so its door must not open meanwhile.
	if(m_DoorSwitch < 0 || m_aAttempts[pChr->GetPlayer()->GetCid()].m_DoorIndex < 0)
		return;
	SSwitchers &Switcher = GameServer()->Switchers()[m_DoorSwitch];
	if(Switcher.m_aStatus[pChr->Team()] && Switcher.m_aType[pChr->Team()] == TILE_SWITCHTIMEDOPEN)
		++Switcher.m_aEndTick[pChr->Team()];
}

void CGameControllerGTrain::OnCharacterSpawn(CCharacter *pChr)
{
	CGameControllerDDNet::OnCharacterSpawn(pChr);

	// the character is not fully initialized yet, place it in the tick
	m_aPendingPlace[pChr->GetPlayer()->GetCid()] = true;
}

int CGameControllerGTrain::FightSize(int Group) const
{
	int Size = 0;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		if(m_aFightGroup[ClientId] == Group && GameServer()->m_apPlayers[ClientId])
			++Size;
	return Size;
}

int CGameControllerGTrain::GoalDistance(int Group) const
{
	return m_aSettings[Group].m_GoalDistance ? m_aSettings[Group].m_GoalDistance : g_Config.m_SvGtrainGoalDistance;
}

bool CGameControllerGTrain::CheckFightLeader(int ClientId, const char *pAction)
{
	if(m_aFightGroup[ClientId] == ClientId)
		return true;
	char aMessage[128];
	str_format(aMessage, sizeof(aMessage), "Only the fight leader can %s.", pAction);
	GameServer()->SendChatTarget(ClientId, aMessage);
	return false;
}

void CGameControllerGTrain::SendFightChat(int Group, const char *pMessage)
{
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		if(m_aFightGroup[ClientId] == Group && GameServer()->m_apPlayers[ClientId])
			GameServer()->SendChatTarget(ClientId, pMessage);
}

void CGameControllerGTrain::KeepWeapons(CCharacter *pChr)
{
	// Giving them every tick also keeps ninja from running out.
	const int Weapons = m_aSettings[m_aFightGroup[pChr->GetPlayer()->GetCid()]].m_Weapons;
	for(int Weapon = WEAPON_SHOTGUN; Weapon <= WEAPON_NINJA; ++Weapon)
		if(Weapons & (1 << Weapon))
			pChr->GiveWeapon(Weapon);
}

int CGameControllerGTrain::SnapPlayerScore(int SnappingClient, CPlayer *pPlayer)
{
	const int ClientId = pPlayer->GetCid();
	return pPlayer->GetTeam() != TEAM_SPECTATORS && m_aFightTeam[m_aFightGroup[ClientId]] != 0 ? m_aFightWins[ClientId] : 0;
}

void CGameControllerGTrain::UpdateFightTeams()
{
	int aSizes[MAX_CLIENTS] = {};
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		if(GameServer()->m_apPlayers[ClientId] && GameServer()->m_apPlayers[ClientId]->GetTeam() != TEAM_SPECTATORS)
			++aSizes[m_aFightGroup[ClientId]];
	for(int Group = 0; Group < MAX_CLIENTS; ++Group)
		if(aSizes[Group] < 2)
			m_aFightTeam[Group] = 0;
	for(int Group = 0; Group < MAX_CLIENTS; ++Group)
	{
		if(aSizes[Group] < 2 || m_aFightTeam[Group] != 0)
			continue;
		for(int Team = 1; Team < TEAM_SUPER; ++Team)
		{
			if(std::find(std::begin(m_aFightTeam), std::end(m_aFightTeam), Team) == std::end(m_aFightTeam))
			{
				m_aFightTeam[Group] = Team;
				break;
			}
		}
	}
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
		m_aScoreboardTeams[ClientId] = pPlayer && pPlayer->GetTeam() != TEAM_SPECTATORS ? m_aFightTeam[m_aFightGroup[ClientId]] : 0;
		if(pPlayer)
		{
			pPlayer->SetNameMarked(m_aScoreboardTeams[ClientId] != 0 && m_aFightGroup[ClientId] == ClientId);
			Server()->SetClientScore(ClientId, SnapPlayerScore(ClientId, pPlayer));
		}
	}
	Teams().SetScoreboardTeams(m_aScoreboardTeams);
}

void CGameControllerGTrain::RestartFight(int Group)
{
	m_aFightRestart[Group] = false;
	if(m_vTrainPositions.empty())
		return;
	const int Start = PathNode(m_vTrainPositions[secure_rand_below(m_vTrainPositions.size())]);
	const vec2 Pos = m_PathGraph.m_vNodes[Start].m_Pos;
	const int TargetDistance = GoalDistance(Group);
	// All owners of this goal belong to the resetting group. Build it once,
	// then share it while keeping timers local to each player.
	auto pGoal = m_aAttempts[Group].m_pGoal;
	if(!pGoal)
		pGoal = std::make_shared<CGTrainGoal>();
	if(m_aSettings[Group].m_Freeplay)
		*pGoal = CGTrainGoal();
	else
		m_Pathfinder.Build(*pGoal, Start, TargetDistance, secure_rand_below(1 << 30));
	m_SyncFightDeaths = true;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
		if(m_aFightGroup[ClientId] != Group || !pPlayer || pPlayer->GetTeam() == TEAM_SPECTATORS)
			continue;
		CCharacter *pChr = pPlayer->GetCharacter();
		if(!pChr)
		{
			// Delete a dead character before reusing its allocation pool slot.
			pPlayer->KillCharacter(WEAPON_GAME, false);
			pChr = pPlayer->ForceSpawn(Pos);
			pChr->SetSolo(true);
		}
		PlaceForAttempt(pChr, pGoal, Start, TargetDistance, false);
	}
	m_SyncFightDeaths = false;
}

bool CGameControllerGTrain::LeaveFight(int ClientId, bool NewAttempt)
{
	const int Group = m_aFightGroup[ClientId];
	if(FightSize(Group) <= 1)
	{
		m_aFightWins[ClientId] = 0;
		return false;
	}
	const bool Restart = m_aFightRestart[Group];
	const CSettings Settings = m_aSettings[Group];
	const int ScoreboardTeam = m_aFightTeam[Group];
	m_aFightRestart[Group] = false;
	m_aFightTeam[Group] = 0;
	int RemainingGroup = Group == ClientId ? -1 : Group;
	if(RemainingGroup < 0)
		for(int Other = 0; Other < MAX_CLIENTS; ++Other)
			if(Other != ClientId && m_aFightGroup[Other] == Group && GameServer()->m_apPlayers[Other])
			{
				RemainingGroup = Other;
				break;
			}
	if(RemainingGroup >= 0)
	{
		for(int Other = 0; Other < MAX_CLIENTS; ++Other)
			if(Other != ClientId && m_aFightGroup[Other] == Group)
				m_aFightGroup[Other] = RemainingGroup;
		m_aFightRestart[RemainingGroup] = Restart;
		m_aFightTeam[RemainingGroup] = ScoreboardTeam;
		m_aSettings[RemainingGroup] = Settings;
	}
	m_aFightGroup[ClientId] = ClientId;
	m_aFightRestart[ClientId] = false;
	m_aFightWins[ClientId] = 0;
	m_aSettings[ClientId] = Settings;
	// A fight ends when only one player is left, so does their score.
	if(RemainingGroup >= 0 && FightSize(RemainingGroup) <= 1)
		m_aFightWins[RemainingGroup] = 0;
	m_aAttempts[ClientId].m_pGoal.reset();
	if(NewAttempt)
		m_aPendingPlace[ClientId] = true;
	UpdateFightTeams();
	return true;
}

void CGameControllerGTrain::Fight(int ClientId, const char *pName)
{
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(!pPlayer)
		return;
	if(!pName[0])
	{
		GameServer()->SendChatTarget(ClientId, LeaveFight(ClientId, true) ? "You left fight mode." : "You are not in fight mode.");
		return;
	}
	const int Target = GameServer()->FindClientIdByName(pName).value_or(-1);
	if(Target < 0 || !GameServer()->m_apPlayers[Target])
	{
		GameServer()->SendChatTarget(ClientId, "Player not found. Use their full name.");
		return;
	}
	if(Target == ClientId)
	{
		GameServer()->SendChatTarget(ClientId, "You cannot fight yourself.");
		return;
	}
	CPlayer *pTarget = GameServer()->m_apPlayers[Target];
	// Paused and /spec players keep their characters and can fight.
	if(pPlayer->GetTeam() == TEAM_SPECTATORS || pTarget->GetTeam() == TEAM_SPECTATORS)
	{
		GameServer()->SendChatTarget(ClientId, "Spectators cannot join a fight.");
		return;
	}
	if(m_vTrainPositions.empty())
	{
		GameServer()->SendChatTarget(ClientId, "Training is disabled on this map.");
		return;
	}
	const int SourceGroup = m_aFightGroup[ClientId];
	const int TargetGroup = m_aFightGroup[Target];
	if(SourceGroup == TargetGroup)
	{
		GameServer()->SendChatTarget(ClientId, "You are already in the same fight.");
		return;
	}
	// Joining players enter the target group's current attempt, so the target
	// group keeps its positions. Dead joiners are placed there on respawn.
	const CAttempt &TargetAttempt = m_aAttempts[TargetGroup];
	const bool JoinAttempt = TargetAttempt.m_pGoal && TargetAttempt.m_Start >= 0;
	// Joiners adopt the target group's settings, losing weapons it lacks.
	const int RemovedWeapons = m_aSettings[SourceGroup].m_Weapons & ~m_aSettings[TargetGroup].m_Weapons;
	for(int Other = 0; Other < MAX_CLIENTS; ++Other)
	{
		if(m_aFightGroup[Other] != SourceGroup)
			continue;
		m_aFightGroup[Other] = TargetGroup;
		CCharacter *pChr = GameServer()->GetPlayerChar(Other);
		for(int Weapon = WEAPON_SHOTGUN; pChr && Weapon <= WEAPON_NINJA; ++Weapon)
			if((RemovedWeapons & (1 << Weapon)) && pChr->GetWeaponGot(Weapon))
				pChr->GiveWeapon(Weapon, true);
		if(JoinAttempt && pChr)
			PlaceForAttempt(pChr, TargetAttempt.m_pGoal, TargetAttempt.m_Start, TargetAttempt.m_TargetDistance, false);
	}
	m_aFightRestart[SourceGroup] = false;
	if(!JoinAttempt)
		m_aFightRestart[TargetGroup] = true;
	UpdateFightTeams();
	char aMessage[160];
	str_format(aMessage, sizeof(aMessage), "'%s' joined the fight with '%s'. Use /fight to leave.",
		Server()->ClientName(ClientId), Server()->ClientName(Target));
	SendFightChat(TargetGroup, aMessage);
}

void CGameControllerGTrain::Freeplay(int ClientId)
{
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(!pPlayer)
		return;
	const int Group = m_aFightGroup[ClientId];
	if(!CheckFightLeader(ClientId, "toggle freeplay"))
		return;
	if(pPlayer->GetTeam() == TEAM_SPECTATORS || pPlayer->IsPaused())
	{
		GameServer()->SendChatTarget(ClientId, "You must be playing and unpaused to toggle freeplay.");
		return;
	}
	bool &Freeplay = m_aSettings[Group].m_Freeplay;
	Freeplay = !Freeplay;
	for(int Other = 0; Other < MAX_CLIENTS; ++Other)
		if(Freeplay && m_aFightGroup[Other] == Group)
			m_aAttempts[Other].m_vRoute.clear();
	SendFightChat(Group, Freeplay ? "Freeplay enabled. Flags and paths are hidden; captures are disabled." : "Freeplay disabled. Starting a new training attempt.");
	if(!Freeplay)
	{
		if(FightSize(Group) > 1)
			RestartFight(Group);
		else if(CCharacter *pChr = pPlayer->GetCharacter())
			PlaceRandomly(pChr);
		else
			m_aPendingPlace[ClientId] = true;
	}
}

void CGameControllerGTrain::Retry(int ClientId)
{
	CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
	if(!pPlayer)
		return;
	if(pPlayer->GetTeam() == TEAM_SPECTATORS || pPlayer->IsPaused())
	{
		GameServer()->SendChatTarget(ClientId, "You must be playing and unpaused to retry.");
		return;
	}
	const int Group = m_aFightGroup[ClientId];
	const CAttempt &Attempt = m_aAttempts[Group];
	if(!Attempt.m_pGoal || Attempt.m_Start < 0)
	{
		GameServer()->SendChatTarget(ClientId, "There is no training attempt to retry yet.");
		return;
	}
	// A leader retry also cancels a pending new attempt after their death.
	// Peers keep their own state and respawn normally at the current goal.
	if(Group == ClientId)
		m_aFightRestart[Group] = false;
	CCharacter *pChr = pPlayer->GetCharacter();
	if(!pChr)
	{
		pPlayer->KillCharacter(WEAPON_GAME, false);
		pChr = pPlayer->ForceSpawn(m_PathGraph.m_vNodes[Attempt.m_Start].m_Pos);
		pChr->SetSolo(true);
	}
	PlaceForAttempt(pChr, Attempt.m_pGoal, Attempt.m_Start, Attempt.m_TargetDistance, true);
}

void CGameControllerGTrain::Distance(int ClientId, std::optional<int> Tiles)
{
	if(!GameServer()->m_apPlayers[ClientId])
		return;
	const int Group = m_aFightGroup[ClientId];
	char aMessage[160];
	if(!Tiles)
	{
		str_format(aMessage, sizeof(aMessage), "Goal distance: %d tiles%s. Use /distance 1-%d to change it, 0 for the server default.",
			GoalDistance(Group), m_aSettings[Group].m_GoalDistance ? "" : " (server default)", (int)MAX_GOAL_DISTANCE);
		GameServer()->SendChatTarget(ClientId, aMessage);
		return;
	}
	if(!CheckFightLeader(ClientId, "change the goal distance"))
		return;
	if(*Tiles < 0 || *Tiles > MAX_GOAL_DISTANCE)
	{
		str_format(aMessage, sizeof(aMessage), "The goal distance must be between 1 and %d tiles, 0 for the server default.", (int)MAX_GOAL_DISTANCE);
		GameServer()->SendChatTarget(ClientId, aMessage);
		return;
	}
	// The current goal stays, so retries keep their distance.
	m_aSettings[Group].m_GoalDistance = *Tiles;
	str_format(aMessage, sizeof(aMessage), "Goal distance set to %d tiles%s for new goals.", GoalDistance(Group), *Tiles ? "" : " (server default)");
	SendFightChat(Group, aMessage);
}

void CGameControllerGTrain::NoDoubleJump(int ClientId)
{
	if(!GameServer()->m_apPlayers[ClientId] || !CheckFightLeader(ClientId, "toggle /nodj"))
		return;
	bool &NoDoubleJump = m_aSettings[ClientId].m_NoDoubleJump;
	NoDoubleJump = !NoDoubleJump;
	SendFightChat(ClientId, NoDoubleJump ? "Attempts now start without double jump. Touching the ground gives it back." : "Attempts now start with double jump.");
}

void CGameControllerGTrain::ScoreLimit(int ClientId, std::optional<int> Limit)
{
	if(!GameServer()->m_apPlayers[ClientId])
		return;
	const int Group = m_aFightGroup[ClientId];
	char aMessage[160];
	if(!Limit)
	{
		char aLimit[16] = "none";
		if(m_aSettings[Group].m_ScoreLimit > 0)
			str_format(aLimit, sizeof(aLimit), "%d", m_aSettings[Group].m_ScoreLimit);
		str_format(aMessage, sizeof(aMessage), "Fight score limit: %s. Use /scorelimit 1-%d to change it, 0 for no limit.", aLimit, (int)MAX_SCORE_LIMIT);
		GameServer()->SendChatTarget(ClientId, aMessage);
		return;
	}
	if(!CheckFightLeader(ClientId, "change the score limit"))
		return;
	if(*Limit < 0 || *Limit > MAX_SCORE_LIMIT)
	{
		str_format(aMessage, sizeof(aMessage), "The score limit must be between 1 and %d, 0 for no limit.", (int)MAX_SCORE_LIMIT);
		GameServer()->SendChatTarget(ClientId, aMessage);
		return;
	}
	m_aSettings[Group].m_ScoreLimit = *Limit;
	if(*Limit > 0)
		str_format(aMessage, sizeof(aMessage), "The first to %d points wins the fight.", *Limit);
	else
		str_copy(aMessage, "The fight score limit is disabled.");
	SendFightChat(Group, aMessage);
}

void CGameControllerGTrain::ToggleWeapon(int ClientId, int Weapon)
{
	static const char *const s_apNames[NUM_WEAPONS] = {"hammer", "gun", "shotgun", "grenade", "laser", "ninja"};
	const char *pName = s_apNames[Weapon];
	char aMessage[128];
	str_format(aMessage, sizeof(aMessage), "toggle /%s", pName);
	if(!GameServer()->m_apPlayers[ClientId] || !CheckFightLeader(ClientId, aMessage))
		return;
	int &Weapons = m_aSettings[ClientId].m_Weapons;
	Weapons ^= 1 << Weapon;
	const bool Enabled = Weapons & (1 << Weapon);
	for(int Other = 0; Other < MAX_CLIENTS; ++Other)
	{
		CCharacter *pChr = GameServer()->GetPlayerChar(Other);
		if(m_aFightGroup[Other] == ClientId && pChr && pChr->GetWeaponGot(Weapon) != Enabled)
			pChr->GiveWeapon(Weapon, !Enabled);
	}
	if(Enabled)
		str_format(aMessage, sizeof(aMessage), "You keep the %s in every attempt until /%s is used again.", pName, pName);
	else
		str_format(aMessage, sizeof(aMessage), "The %s was removed.", pName);
	SendFightChat(ClientId, aMessage);
}

void CGameControllerGTrain::Help()
{
	log_info("chatresp", "GTrain commands:");
	log_info("chatresp", "Kill: new position and flag");
	log_info("chatresp", "/r or /retry: retry the current position and flag");
	log_info("chatresp", "/fight <player>: join a player's fight, /fight alone leaves it");
	log_info("chatresp", "/distance <1-%d>: goal distance in tiles, 0 for the server default", (int)MAX_GOAL_DISTANCE);
	log_info("chatresp", "/nodj: toggle starting attempts without double jump");
	log_info("chatresp", "/scorelimit <0-%d>: points to win a fight, 0 for no limit", (int)MAX_SCORE_LIMIT);
	log_info("chatresp", "/shotgun, /grenade, /laser, /ninja: toggle keeping that weapon");
	log_info("chatresp", "/freeplay: toggle training without flags and paths");
	log_info("chatresp", "In a fight, only the leader (*name*) changes these settings.");
}

std::optional<int> CGameControllerGTrain::RaceTimerStartTick(CCharacter *pChr)
{
	const int ClientId = pChr->GetPlayer()->GetCid();
	const CAttempt &Attempt = m_aAttempts[ClientId];
	if(Attempt.m_Start < 0)
		return CGameControllerDDNet::RaceTimerStartTick(pChr);
	// Every attempt times itself: zero while hovering, counting afterwards.
	if(m_aPendingPlace[ClientId] || Attempt.m_StartTick < 0)
		return Server()->Tick();
	return Attempt.m_StartTick;
}

int CGameControllerGTrain::OnCharacterDeath(CCharacter *pVictim, CPlayer *pKiller, int Weapon)
{
	const int Result = CGameControllerDDNet::OnCharacterDeath(pVictim, pKiller, Weapon);
	const int ClientId = pVictim->GetPlayer()->GetCid();
	const int Group = m_aFightGroup[ClientId];
	if(m_SyncFightDeaths || !pVictim->IsAlive() || ClientId != Group || FightSize(Group) <= 1)
		return Result;
	m_aFightRestart[Group] = true;
	// Death callbacks run before the victim is marked dead. Suppress recursive
	// propagation while killing its peers; never delete the original victim.
	m_SyncFightDeaths = true;
	for(int Other = 0; Other < MAX_CLIENTS; ++Other)
		if(Other != ClientId && m_aFightGroup[Other] == Group && GameServer()->GetPlayerChar(Other))
			GameServer()->m_apPlayers[Other]->KillCharacter(WEAPON_SELF);
	m_SyncFightDeaths = false;
	return Result;
}

int CGameControllerGTrain::PathNode(vec2 Pos) const
{
	// Do not use the collision lookup's clamping outside the map.
	const int Width = GameServer()->Collision()->GetWidth();
	const int Height = GameServer()->Collision()->GetHeight();
	if(Pos.x < 0 || Pos.y < 0 || Pos.x >= Width * 32.0f || Pos.y >= Height * 32.0f)
		return -1;
	return m_vTileNodes[(int)(Pos.y / 32) * Width + (int)(Pos.x / 32)];
}

void CGameControllerGTrain::UpdatePath(CCharacter *pChr)
{
	CAttempt &Attempt = m_aAttempts[pChr->GetPlayer()->GetCid()];
	if(!Attempt.m_pGoal || !Attempt.m_pGoal->m_Ready)
		return;
	const int Node = Attempt.m_StartTick < 0 ? Attempt.m_Start : PathNode(pChr->m_Pos);
	if(Node == Attempt.m_RouteNode)
		return;
	if(Node < 0)
	{
		Attempt.m_RouteNode = -1;
		return;
	}
	// Normal movement and small backwards steps keep the drawn route stable.
	const size_t Begin = Attempt.m_RouteIndex > 8 ? Attempt.m_RouteIndex - 8 : 0;
	const size_t End = std::min(Attempt.m_vRoute.size(), Attempt.m_RouteIndex + 9);
	for(size_t i = Begin; i < End; ++i)
		if(Attempt.m_vRoute[i] == Node)
		{
			Attempt.m_RouteIndex = i;
			Attempt.m_RouteNode = Node;
			return;
		}
	// Deviations follow the goal's distance field, costing no search. Far
	// beyond the field the route stays hidden until the player returns.
	m_Pathfinder.FindPath(*Attempt.m_pGoal, Node, Attempt.m_vRoute);
	Attempt.m_RouteIndex = 0;
	Attempt.m_RouteNode = Node;
}

void CGameControllerGTrain::AnnounceFightScore(int Winner)
{
	const int Group = m_aFightGroup[Winner];
	const int ScoreLimit = m_aSettings[Group].m_ScoreLimit;
	const int Fails = m_aAttempts[Winner].m_Fails;
	++m_aFightWins[Winner];
	Server()->SetClientScore(Winner, SnapPlayerScore(Winner, GameServer()->m_apPlayers[Winner]));
	char aScore[64] = "Fight score:";
	if(ScoreLimit > 0)
		str_format(aScore, sizeof(aScore), "Fight score (first to %d):", ScoreLimit);
	char aMessage[MAX_CHAT_LENGTH];
	str_format(aMessage, sizeof(aMessage), "'%s' captured the flag (%d fail%s). %s", Server()->ClientName(Winner), Fails, Fails == 1 ? "" : "s", aScore);
	bool First = true;
	// Leader first, then the remaining members. Split long standings into
	// complete chat messages rather than truncating names or scores.
	for(int i = -1; i < MAX_CLIENTS; ++i)
	{
		const int Other = i < 0 ? Group : i;
		if(i == Group || m_aFightGroup[Other] != Group || !GameServer()->m_apPlayers[Other])
			continue;
		char aEntry[128];
		str_format(aEntry, sizeof(aEntry), "%s '%s': %d", First ? "" : ",", Server()->ClientName(Other), m_aFightWins[Other]);
		if(str_length(aMessage) + str_length(aEntry) >= (int)sizeof(aMessage))
		{
			GameServer()->SendChat(-1, TEAM_ALL, aMessage);
			str_copy(aMessage, aScore);
			str_format(aEntry, sizeof(aEntry), " '%s': %d", Server()->ClientName(Other), m_aFightWins[Other]);
		}
		str_append(aMessage, aEntry);
		First = false;
	}
	GameServer()->SendChat(-1, TEAM_ALL, aMessage);

	if(ScoreLimit <= 0 || m_aFightWins[Winner] < ScoreLimit)
		return;
	str_format(aMessage, sizeof(aMessage), "'%s' won the fight with %d points! The fight score starts over.", Server()->ClientName(Winner), m_aFightWins[Winner]);
	GameServer()->SendChat(-1, TEAM_ALL, aMessage);
	for(int Other = 0; Other < MAX_CLIENTS; ++Other)
	{
		if(m_aFightGroup[Other] != Group || !GameServer()->m_apPlayers[Other])
			continue;
		m_aFightWins[Other] = 0;
		Server()->SetClientScore(Other, SnapPlayerScore(Other, GameServer()->m_apPlayers[Other]));
	}
}

void CGameControllerGTrain::CheckGoal(CCharacter *pChr)
{
	const int ClientId = pChr->GetPlayer()->GetCid();
	CAttempt &Attempt = m_aAttempts[ClientId];
	const CGTrainGoal &Target = *Attempt.m_pGoal;

	const vec2 Goal = m_PathGraph.m_vNodes[Target.m_Goal].m_Pos;
	// Check the movement segment too, so a fast tee cannot skip the flag.
	// A teleport's long displacement must not collect a flag along the way.
	vec2 Closest = pChr->m_Pos;
	const vec2 Movement = pChr->m_Pos - pChr->m_PrevPos;
	const float MovementSquared = dot(Movement, Movement);
	if(MovementSquared > 0 && MovementSquared <= 256.0f * 256.0f)
		Closest = pChr->m_PrevPos + Movement * std::clamp(dot(Goal - pChr->m_PrevPos, Movement) / MovementSquared, 0.0f, 1.0f);
	const vec2 GoalOffset = Closest - Goal;
	if(dot(GoalOffset, GoalOffset) < 42.0f * 42.0f)
	{
		const int Group = m_aFightGroup[ClientId];
		if(FightSize(Group) > 1)
		{
			AnnounceFightScore(ClientId);
			RestartFight(Group);
		}
		else
		{
			char aMessage[256];
			const double Seconds = (Server()->Tick() - Attempt.m_StartTick) / (double)Server()->TickSpeed();
			str_format(aMessage, sizeof(aMessage), "'%s' captured the flag in %.2f seconds (%d fail%s).", Server()->ClientName(ClientId), Seconds, Attempt.m_Fails, Attempt.m_Fails == 1 ? "" : "s");
			GameServer()->SendChat(-1, TEAM_ALL, aMessage);
			PlaceRandomly(pChr);
		}
		for(int Other = 0; Other < MAX_CLIENTS; ++Other)
		{
			CCharacter *pOther = GameServer()->GetPlayerChar(Other);
			if(m_aFightGroup[Other] != Group || !pOther)
				continue;
			if(Server()->IsSixup(Other))
				GameServer()->CreateSound(pOther->m_Pos, SOUND_CTF_CAPTURE, CClientMask().set(Other));
			else
				GameServer()->CreateSoundGlobal(SOUND_CTF_CAPTURE, Other);
		}
	}
}

void CGameControllerGTrain::OnPlayerDisconnect(CPlayer *pPlayer, const char *pReason)
{
	const int ClientId = pPlayer->GetCid();
	LeaveFight(ClientId, false);
	m_aPendingPlace[ClientId] = false;
	m_aAttempts[ClientId] = CAttempt();
	m_aFightWins[ClientId] = 0;
	m_aSettings[ClientId] = CSettings();
	CGameControllerDDNet::OnPlayerDisconnect(pPlayer, pReason);
}

void CGameControllerGTrain::OnPlayerConnect(CPlayer *pPlayer)
{
	CGameControllerDDNet::OnPlayerConnect(pPlayer);
	UpdateFightTeams();
}

void CGameControllerGTrain::Snap(int SnappingClient)
{
	CGameControllerDDNet::Snap(SnappingClient);
	if(SnappingClient == SERVER_DEMO_CLIENT)
		return;
	CPlayer *pPlayer = GameServer()->m_apPlayers[SnappingClient];
	if(!pPlayer)
		return;
	int ClientId = SnappingClient;
	if((pPlayer->GetTeam() == TEAM_SPECTATORS || pPlayer->IsPaused()) && pPlayer->SpectatorId() != SPEC_FREEVIEW)
		ClientId = pPlayer->SpectatorId();
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return;
	CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
	// Only the held player gets their door, as a short laser on its tile.
	const CAttempt &Attempt = m_aAttempts[ClientId];
	if(pChr && Attempt.m_DoorIndex >= 0 && Attempt.m_StartTick < 0 && m_aDoorSnapIds[ClientId] &&
		GameServer()->Switchers()[m_DoorSwitch].m_aStatus[pChr->Team()])
	{
		const vec2 DoorPos = GameServer()->Collision()->GetPos(Attempt.m_DoorIndex);
		const CSnapContext Context(GameServer()->GetClientVersion(SnappingClient), Server()->IsSixup(SnappingClient), SnappingClient);
		GameServer()->SnapLaserObject(Context, *m_aDoorSnapIds[ClientId], DoorPos - vec2(12, 0), DoorPos + vec2(12, 0), -1, -1, LASERTYPE_DOOR, 0, m_DoorSwitch);
	}
	if(m_aSettings[m_aFightGroup[ClientId]].m_Freeplay)
		return;
	if(!pChr || !Attempt.m_pGoal || !Attempt.m_pGoal->m_Ready || !m_FlagSnapId)
		return;
	const vec2 Goal = m_PathGraph.m_vNodes[Attempt.m_pGoal->m_Goal].m_Pos;
	CNetObj_Flag Flag = {};
	Flag.m_X = (int)Goal.x;
	Flag.m_Y = (int)Goal.y;
	Flag.m_Team = TEAM_BLUE;
	Server()->SnapNewItem(*m_FlagSnapId, Flag);

	if(Attempt.m_RouteNode < 0 || Attempt.m_vRoute.empty())
		return;
	int NumParticles = 0;
	const float ClearRadius = g_Config.m_SvGtrainPathClearRadius;
	const float Spacing = g_Config.m_SvGtrainPathParticleSpacing;
	float DistanceToNext = Spacing;
	std::optional<vec2> LastParticle;
	const auto Emit = [&](vec2 Pos) {
		// Leave gameplay space clear, including where a winding path comes
		// back near the player. Only snapshot particles inside the viewport.
		Pos = vec2(round_to_int(Pos.x), round_to_int(Pos.y));
		const vec2 Offset = Pos - pChr->m_Pos;
		if(dot(Offset, Offset) < ClearRadius * ClearRadius || NetworkClipped(GameServer(), SnappingClient, Pos) || NumParticles == NUM_DIRECTION_PARTICLES)
			return;
		const auto SnapId = m_aDirectionSnapIds[NumParticles++];
		if(!SnapId)
			return;
		CNetObj_Projectile Particle = {};
		Particle.m_X = round_to_int(Pos.x);
		Particle.m_Y = round_to_int(Pos.y);
		// Hammer projectiles have no sprite but use the same bullet trail as
		// the gun. Zero velocity keeps them still and skips client prediction.
		Particle.m_Type = WEAPON_HAMMER;
		Server()->SnapNewItem(*SnapId, Particle);
		LastParticle = Pos;
	};
	// Carry the spacing across tile boundaries and turns. Sampling each tile
	// separately would still produce at least one particle per tile.
	const auto SampleSegment = [&](vec2 From, vec2 To) {
		const float Length = distance(From, To);
		if(Length == 0)
			return;
		while(DistanceToNext <= Length && NumParticles < NUM_DIRECTION_PARTICLES)
		{
			Emit(mix(From, To, DistanceToNext / Length));
			DistanceToNext += Spacing;
		}
		DistanceToNext -= Length;
	};
	vec2 Incoming = m_PathGraph.m_vNodes[Attempt.m_vRoute[Attempt.m_RouteIndex]].m_Pos;
	// Bounded traversal and a shared snapshot-ID pool: no per-snapshot heap
	// allocations or map searches, even with very distant goals.
	for(int Step = 0; Step < MAX_PATH_NODES && NumParticles < NUM_DIRECTION_PARTICLES; ++Step)
	{
		const size_t Index = Attempt.m_RouteIndex + Step;
		if(Index >= Attempt.m_vRoute.size())
			break;
		const int Node = Attempt.m_vRoute[Index];
		const vec2 Center = m_PathGraph.m_vNodes[Node].m_Pos;
		const int Next = Index + 1 < Attempt.m_vRoute.size() ? Attempt.m_vRoute[Index + 1] : -1;
		if(Next < 0)
		{
			if(Node == Attempt.m_pGoal->m_Goal)
			{
				SampleSegment(Incoming, Center);
				// Keep an endpoint even for a route shorter than the spacing.
				if(!LastParticle || *LastParticle != Center)
					Emit(Center);
			}
			break;
		}
		if(m_PathGraph.IsTeleportStep(Node, Next))
		{
			// A teleport is a discontinuity, not a line across the map.
			Incoming = m_PathGraph.m_vNodes[Next].m_Pos;
			DistanceToNext = Spacing;
			continue;
		}
		const vec2 Outgoing = (Center + m_PathGraph.m_vNodes[Next].m_Pos) * 0.5f;
		// Round turns through each tile center. This curve stays within the
		// traversable tiles and avoids a staircase of right-angle corners.
		const vec2 In = Center - Incoming, Out = Outgoing - Center;
		if(In.x * Out.y == In.y * Out.x)
		{
			SampleSegment(Incoming, Outgoing);
		}
		else
		{
			// Four short segments approximate the rounded turn safely inside
			// its tile; only points at the configured spacing are emitted.
			vec2 Previous = Incoming;
			for(int i = 1; i <= 4; ++i)
			{
				const float t = i * 0.25f;
				const vec2 Pos = mix(mix(Incoming, Center, t), mix(Center, Outgoing, t), t);
				SampleSegment(Previous, Pos);
				Previous = Pos;
			}
		}
		Incoming = Outgoing;
	}
}

void CGameControllerGTrain::Tick()
{
	CGameControllerDDNet::Tick();

	// killing is how players choose a new position, never delay it
	g_Config.m_SvKillDelay = 0;
	if(IsGamePaused())
	{
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			if(m_aAttempts[ClientId].m_StartTick >= 0)
				++m_aAttempts[ClientId].m_StartTick;
			if(CCharacter *pChr = GameServer()->GetPlayerChar(ClientId))
			{
				KeepWeapons(pChr);
				DelayHoverDoor(pChr);
			}
		}
		return;
	}

	// Remove spectator links before group respawns, and coalesce all death
	// and spawn requests into one goal selection and one placement per group.
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		CPlayer *pPlayer = GameServer()->m_apPlayers[ClientId];
		if(!pPlayer)
			continue;
		if(pPlayer->GetTeam() == TEAM_SPECTATORS)
			LeaveFight(ClientId, false);
		else if(m_aPendingPlace[ClientId] && m_aFightGroup[ClientId] == ClientId && FightSize(ClientId) > 1)
			m_aFightRestart[m_aFightGroup[ClientId]] = true;
	}
	for(int Group = 0; Group < MAX_CLIENTS; ++Group)
		if(m_aFightRestart[Group])
			RestartFight(Group);

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		CCharacter *pChr = GameServer()->GetPlayerChar(ClientId);
		if(!pChr)
			continue;
		KeepWeapons(pChr);
		if(pChr->IsPaused())
		{
			if(m_aAttempts[ClientId].m_StartTick >= 0)
				++m_aAttempts[ClientId].m_StartTick;
			DelayHoverDoor(pChr);
			continue;
		}

		if(m_aPendingPlace[ClientId])
		{
			const int Group = m_aFightGroup[ClientId];
			const CAttempt &GroupAttempt = m_aAttempts[Group];
			// A member's own respawn rejoins the current attempt without
			// moving peers or rebuilding their shared goal. Players already
			// in that attempt count it as a fail, joiners start fresh.
			if(Group != ClientId && FightSize(Group) > 1 && GroupAttempt.m_pGoal && GroupAttempt.m_Start >= 0)
				PlaceForAttempt(pChr, GroupAttempt.m_pGoal, GroupAttempt.m_Start, GroupAttempt.m_TargetDistance, true);
			else
				PlaceRandomly(pChr);
			continue;
		}
		CAttempt &Attempt = m_aAttempts[ClientId];

		// Standing on the door gives the double jump back, /nodj takes it.
		if(Attempt.m_StartTick < 0 && Attempt.m_DoorIndex >= 0 && m_aSettings[m_aFightGroup[ClientId]].m_NoDoubleJump)
			pChr->UseUpAirJumps();
		// hover is over, let the player play again
		if(Attempt.m_StartTick < 0 && pChr->m_FreezeTime == 0)
		{
			if(pChr->m_ZeroGravity)
				pChr->SetZeroGravity(false);
			Attempt.m_StartTick = Server()->Tick();
		}
		if(m_aSettings[m_aFightGroup[ClientId]].m_Freeplay)
			continue;
		UpdatePath(pChr);
		if(Attempt.m_StartTick >= 0 && Attempt.m_pGoal && Attempt.m_pGoal->m_Ready)
			CheckGoal(pChr);
	}
}
