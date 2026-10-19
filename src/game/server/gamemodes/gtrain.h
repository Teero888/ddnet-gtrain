#ifndef GAME_SERVER_GAMEMODES_GTRAIN_H
#define GAME_SERVER_GAMEMODES_GTRAIN_H

#include "ddnet.h"
#include "gtrain_path.h"

#include <base/vmath.h>

#include <engine/shared/protocol.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

// Gores training: players get placed at random positions between
// the start and the finish line, killing places them again.
class CGameControllerGTrain : public CGameControllerDDNet
{
	enum
	{
		HOVER_TICKS = 50,
		MAX_FREEZE_DISTANCE = 20,
		NUM_DIRECTION_PARTICLES = 256,
		MAX_PATH_NODES = 256,
		MAX_GOAL_DISTANCE = 500,
		DEFAULT_SCORE_LIMIT = 10,
		MAX_SCORE_LIMIT = 1000,
	};

	// centers of all non-freeze tiles between the start and the finish line
	std::vector<vec2> m_vTrainPositions;
	bool m_aPendingPlace[MAX_CLIENTS];
	CGTrainPathGraph m_PathGraph;
	std::vector<int> m_vTileNodes;
	CGTrainPathfinder m_Pathfinder;
	struct CAttempt
	{
		std::shared_ptr<CGTrainGoal> m_pGoal;
		int m_Start = -1;
		int m_TargetDistance = 0;
		int m_StartTick = -1;
		// retries of this start and goal, after death or /r
		int m_Fails = 0;
		std::vector<int> m_vRoute;
		size_t m_RouteIndex = 0;
		int m_RouteNode = -1;
	};
	// Fight leaders choose these for their whole group. Leaving players and
	// promoted leaders keep the group's settings.
	struct CSettings
	{
		bool m_Freeplay = false;
		bool m_NoDoubleJump = false;
		int m_GoalDistance = 0; // 0 uses sv_gtrain_goal_distance
		int m_ScoreLimit = DEFAULT_SCORE_LIMIT; // 0 disables the limit
		int m_Weapons = 0; // bit per weapon kept for good
	};
	CAttempt m_aAttempts[MAX_CLIENTS];
	int m_aFightGroup[MAX_CLIENTS];
	int m_aFightTeam[MAX_CLIENTS] = {};
	int m_aScoreboardTeams[MAX_CLIENTS] = {};
	bool m_aFightRestart[MAX_CLIENTS] = {};
	int m_aFightWins[MAX_CLIENTS] = {};
	CSettings m_aSettings[MAX_CLIENTS];
	bool m_SyncFightDeaths = false;
	std::optional<int> m_FlagSnapId;
	std::array<std::optional<int>, NUM_DIRECTION_PARTICLES> m_aDirectionSnapIds;

	void FindTrainPositions();
	void PlaceRandomly(class CCharacter *pChr);
	void PlaceForAttempt(class CCharacter *pChr, const std::shared_ptr<CGTrainGoal> &pGoal, int Start, int TargetDistance, bool Retry);
	void RestartFight(int Group);
	bool LeaveFight(int ClientId, bool NewAttempt);
	int FightSize(int Group) const;
	int GoalDistance(int Group) const;
	bool CheckFightLeader(int ClientId, const char *pAction);
	void SendFightChat(int Group, const char *pMessage);
	void KeepWeapons(class CCharacter *pChr);
	void UpdateFightTeams();
	void AnnounceFightScore(int Winner);
	int PathNode(vec2 Pos) const;
	void UpdatePath(class CCharacter *pChr);
	void CheckGoal(class CCharacter *pChr);

public:
	CGameControllerGTrain(class CGameContext *pGameServer);
	~CGameControllerGTrain() override;

	void OnCharacterSpawn(class CCharacter *pChr) override;
	int OnCharacterDeath(class CCharacter *pVictim, class CPlayer *pKiller, int Weapon) override;
	void OnPlayerDisconnect(class CPlayer *pPlayer, const char *pReason) override;
	void OnPlayerConnect(class CPlayer *pPlayer) override;
	int SnapPlayerScore(int SnappingClient, CPlayer *pPlayer) override;
	bool HasTimeScore() const override { return false; }
	CFinishTime SnapPlayerTime(int SnappingClient, CPlayer *pPlayer) override { return CFinishTime::Unset(); }
	void Snap(int SnappingClient) override;
	std::optional<int> RaceTimerStartTick(class CCharacter *pChr) override;
	void Fight(int ClientId, const char *pName);
	void Freeplay(int ClientId);
	void Retry(int ClientId);
	void Distance(int ClientId, std::optional<int> Tiles);
	void NoDoubleJump(int ClientId);
	void ScoreLimit(int ClientId, std::optional<int> Limit);
	void ToggleWeapon(int ClientId, int Weapon);
	void Help();

	void Tick() override;
};
#endif // GAME_SERVER_GAMEMODES_GTRAIN_H
