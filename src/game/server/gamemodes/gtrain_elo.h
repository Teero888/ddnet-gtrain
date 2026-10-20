#ifndef GAME_SERVER_GAMEMODES_GTRAIN_ELO_H
#define GAME_SERVER_GAMEMODES_GTRAIN_ELO_H

#include <algorithm>
#include <cmath>
#include <cstdint>

// GTrain captures earn points: the goal's distance in tiles, worth more on
// every difficulty step of the map and when captured fast. ELO shows a
// player's total points on a logarithmic scale up to MAX_ELO: it grows by the
// same amount whenever they double, which harder maps make them do faster.
namespace GTrainElo
{
	inline constexpr int MAX_ELO = 9999;
	inline constexpr int MAX_STEP = 14; // Extreme with three stars
	inline constexpr double POINTS_PER_TILE = 100; // Easy with one star
	inline constexpr double STEP_MULTIPLIER = 1.5;
	// Typical capture speeds in path tiles per second, from the first captures
	// on the KoG maps. Harder maps are slower, which must not cost points.
	inline constexpr double EASY_TILES_PER_SECOND = 16;
	inline constexpr double EXTREME_TILES_PER_SECOND = 8;
	inline constexpr double MIN_SPEED_FACTOR = 0.5;
	inline constexpr double MAX_SPEED_FACTOR = 2;
	// Up to about this many points, ELO grows linearly with them, so single
	// captures of hard maps don't make new players jump up.
	inline constexpr double LINEAR_POINTS = 50'000'000;

	constexpr double PointsPerTile(int Step)
	{
		double Points = POINTS_PER_TILE;
		for(int i = 0; i < Step; ++i)
			Points *= STEP_MULTIPLIER;
		return Points;
	}

	// The maximum takes 1000 hours of Extreme 3 star captures at 8000 tiles
	// per hour, all at the maximum speed factor. Good players managed about
	// half that pace at a typical speed, so this is completely inhuman.
	inline constexpr double MAX_POINTS = 1000.0 * 8000 * PointsPerTile(MAX_STEP) * MAX_SPEED_FACTOR;

	// The capture's speed relative to the typical one of its difficulty.
	inline double SpeedFactor(int Step, int Distance, double Seconds)
	{
		if(Seconds <= 0)
			return MAX_SPEED_FACTOR;
		const double Typical = EASY_TILES_PER_SECOND + (EXTREME_TILES_PER_SECOND - EASY_TILES_PER_SECOND) * Step / MAX_STEP;
		return std::clamp(Distance / Seconds / Typical, MIN_SPEED_FACTOR, MAX_SPEED_FACTOR);
	}

	inline int64_t CapturePoints(int Step, int Distance, double Seconds)
	{
		return std::llround(Distance * PointsPerTile(Step) * SpeedFactor(Step, Distance, Seconds));
	}

	inline double FromPoints(int64_t Points)
	{
		const double Elo = MAX_ELO * std::log2(1 + Points / LINEAR_POINTS) / std::log2(1 + MAX_POINTS / LINEAR_POINTS);
		return std::min(Elo, (double)MAX_ELO);
	}
}

#endif // GAME_SERVER_GAMEMODES_GTRAIN_ELO_H
