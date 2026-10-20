#include <game/server/gamemodes/gtrain_elo.h>

#include <gtest/gtest.h>

using namespace GTrainElo;

TEST(GTrainElo, PointsPerTileGrowByHalfPerStep)
{
	EXPECT_DOUBLE_EQ(PointsPerTile(0), 100);
	EXPECT_DOUBLE_EQ(PointsPerTile(1), 150);
	EXPECT_DOUBLE_EQ(PointsPerTile(3), 337.5);
	for(int Step = 1; Step <= MAX_STEP; ++Step)
		EXPECT_DOUBLE_EQ(PointsPerTile(Step) / PointsPerTile(Step - 1), 1.5);
	// Extreme with three stars is worth about 292 times Easy with one.
	EXPECT_NEAR(PointsPerTile(MAX_STEP) / PointsPerTile(0), 291.93, 0.01);
}

TEST(GTrainElo, SpeedFactorFollowsTheTypicalSpeedOfTheDifficulty)
{
	// Typical speeds go from 16 tiles per second on Easy to 8 on Extreme.
	EXPECT_DOUBLE_EQ(SpeedFactor(0, 160, 10), 1);
	EXPECT_DOUBLE_EQ(SpeedFactor(7, 120, 10), 1);
	EXPECT_DOUBLE_EQ(SpeedFactor(MAX_STEP, 80, 10), 1);
	EXPECT_DOUBLE_EQ(SpeedFactor(MAX_STEP, 120, 10), 1.5);
	EXPECT_DOUBLE_EQ(SpeedFactor(MAX_STEP, 60, 10), 0.75);
	// Fast captures have no limit, slow ones earn at least half. Instant
	// captures count as taking one tick.
	EXPECT_DOUBLE_EQ(SpeedFactor(MAX_STEP, 1000, 10), 12.5);
	EXPECT_DOUBLE_EQ(SpeedFactor(MAX_STEP, 1, 10), MIN_SPEED_FACTOR);
	EXPECT_DOUBLE_EQ(SpeedFactor(MAX_STEP, 100, 0), 100.0 * (double)SERVER_TICK_SPEED / 8);
	EXPECT_DOUBLE_EQ(SpeedFactor(MAX_STEP, 100, 0), SpeedFactor(MAX_STEP, 100, 1.0 / (double)SERVER_TICK_SPEED));
}

TEST(GTrainElo, CapturePoints)
{
	EXPECT_EQ(CapturePoints(0, 100, 100 / 16.0), 10000);
	EXPECT_EQ(CapturePoints(7, 100, 100 / 12.0), 170859); // 100 * 1.5^7 * 100, rounded
	EXPECT_EQ(CapturePoints(MAX_STEP, 100, 100 / 32.0), std::llround(100 * PointsPerTile(MAX_STEP) * 4));
	EXPECT_EQ(CapturePoints(MAX_STEP, 100, 100), std::llround(100 * PointsPerTile(MAX_STEP) / 2));
}

TEST(GTrainElo, LogarithmicScaleUpToTheMaximum)
{
	EXPECT_EQ(FromPoints(0), 0);
	// Single captures of hard maps barely count for new players.
	EXPECT_LT(FromPoints(CapturePoints(MAX_STEP, 100, 100 / EXTREME_TILES_PER_SECOND)), 150);
	// Every doubling adds the same ELO once past the linear start.
	const double Doubling = FromPoints(200'000'000'000) - FromPoints(100'000'000'000);
	EXPECT_NEAR(FromPoints(400'000'000'000) - FromPoints(200'000'000'000), Doubling, 1);
	EXPECT_NEAR(Doubling, 758, 1);
	// The maximum needs the inhuman amount of points.
	EXPECT_LT(FromPoints((int64_t)(MAX_POINTS * 0.99)), MAX_ELO - 10);
	EXPECT_GT(FromPoints((int64_t)(MAX_POINTS * 0.99)), MAX_ELO - 12);
	EXPECT_EQ(FromPoints((int64_t)(MAX_POINTS * 1.01)), MAX_ELO);
	EXPECT_EQ(FromPoints(INT64_MAX / 2), MAX_ELO);
	int64_t Points = 1;
	for(int i = 0; i < 60; ++i, Points *= 2)
		EXPECT_LE(FromPoints(Points), FromPoints(Points * 2));
}
