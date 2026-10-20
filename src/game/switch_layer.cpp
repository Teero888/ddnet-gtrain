#include "switch_layer.h"

#include "gamecore.h"
#include "mapitems.h"

#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <iterator>
#include <vector>

EAddSwitchLayer AddSwitchLayer(IStorage *pStorage, const char *pPath, int StorageType, const char *pDestination, int DestinationStorageType)
{
	CDataFileReader Reader;
	if(!Reader.Open(pStorage, pPath, StorageType))
		return EAddSwitchLayer::FAILED;
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
			return EAddSwitchLayer::PRESENT;
		if(pLayer->m_Flags & TILESLAYERFLAG_GAME)
		{
			Width = pLayer->m_Width;
			Height = pLayer->m_Height;
		}
	}
	if(Width <= 0 || Height <= 0)
		return EAddSwitchLayer::FAILED;

	CDataFileWriter Writer;
	for(int i = 0; i < Reader.NumItems(); ++i)
	{
		// The writer adds the UUIDs of extended item types itself, like
		// map_resave. Copying them too would duplicate them.
		int Type, Id;
		CUuid Uuid;
		void *pData = Reader.GetItem(i, &Type, &Id, &Uuid);
		if(Type == ITEMTYPE_EX)
			continue;
		Writer.AddItem(Type, Id, Reader.GetItemSize(i), pData, &Uuid);
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

	if(!Writer.Open(pStorage, pDestination, DestinationStorageType))
		return EAddSwitchLayer::FAILED;
	Writer.Finish();
	return EAddSwitchLayer::ADDED;
}
