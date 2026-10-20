#ifndef GAME_SWITCH_LAYER_H
#define GAME_SWITCH_LAYER_H

class IStorage;

enum class EAddSwitchLayer
{
	FAILED,
	PRESENT, // the map has a switch layer already, nothing was written
	ADDED,
};

// Door collision only exists in maps with a switch layer, on the server and
// on clients. Writes the map at pPath to pDestination with an empty switch
// layer added in a group of its own, unless it has one already.
EAddSwitchLayer AddSwitchLayer(IStorage *pStorage, const char *pPath, int StorageType, const char *pDestination, int DestinationStorageType);

#endif // GAME_SWITCH_LAYER_H
