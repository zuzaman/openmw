#include "object.hpp"

#include <unordered_map>

namespace MWLua
{

    std::string idToString(const ObjectId& id)
    {
        return std::to_string(id.mIndex) + "_" + std::to_string(id.mContentFile);
    }

    struct LuaObjectTypeInfo
    {
        std::string_view mName;
        ESM::LuaScriptCfg::Flags mFlag = 0;
    };

    const static std::unordered_map<ESM::RecNameInts, LuaObjectTypeInfo> luaObjectTypeInfo = {
        {ESM::REC_ACTI, {ObjectTypeName::Activator, ESM::LuaScriptCfg::sActivator}},
        {ESM::REC_ARMO, {ObjectTypeName::Armor, ESM::LuaScriptCfg::sArmor}},
        {ESM::REC_BOOK, {ObjectTypeName::Book, ESM::LuaScriptCfg::sBook}},
        {ESM::REC_CLOT, {ObjectTypeName::Clothing, ESM::LuaScriptCfg::sClothing}},
        {ESM::REC_CONT, {ObjectTypeName::Container, ESM::LuaScriptCfg::sContainer}},
        {ESM::REC_CREA, {ObjectTypeName::Creature, ESM::LuaScriptCfg::sCreature}},
        {ESM::REC_DOOR, {ObjectTypeName::Door, ESM::LuaScriptCfg::sDoor}},
        {ESM::REC_INGR, {ObjectTypeName::Ingredient, ESM::LuaScriptCfg::sIngredient}},
        {ESM::REC_LIGH, {ObjectTypeName::Light, ESM::LuaScriptCfg::sLight}},
        {ESM::REC_MISC, {ObjectTypeName::MiscItem, ESM::LuaScriptCfg::sMiscItem}},
        {ESM::REC_NPC_, {ObjectTypeName::NPC, ESM::LuaScriptCfg::sNPC}},
        {ESM::REC_ALCH, {ObjectTypeName::Potion, ESM::LuaScriptCfg::sPotion}},
        {ESM::REC_STAT, {ObjectTypeName::Static}},
        {ESM::REC_WEAP, {ObjectTypeName::Weapon, ESM::LuaScriptCfg::sWeapon}},
        {ESM::REC_APPA, {ObjectTypeName::Apparatus}},
        {ESM::REC_LOCK, {ObjectTypeName::Lockpick}},
        {ESM::REC_PROB, {ObjectTypeName::Probe}},
        {ESM::REC_REPA, {ObjectTypeName::Repair}},
    };

    std::string_view getLuaObjectTypeName(ESM::RecNameInts type, std::string_view fallback)
    {
        auto it = luaObjectTypeInfo.find(type);
        if (it != luaObjectTypeInfo.end())
            return it->second.mName;
        else
            return fallback;
    }

    bool isMarker(const MWWorld::Ptr& ptr)
    {
        std::string_view id = ptr.getCellRef().getRefId();
        return id == "prisonmarker" || id == "divinemarker" || id == "templemarker" || id == "northmarker";
    }

    std::string_view getLuaObjectTypeName(const MWWorld::Ptr& ptr)
    {
        // Behaviour of this function is a part of OpenMW Lua API. We can not just return
        // `ptr.getTypeDescription()` because its implementation is distributed over many files
        // and can be accidentally changed. We use `ptr.getTypeDescription()` only as a fallback
        // for types that are not present in `luaObjectTypeInfo` (for such types result stability
        // is not necessary because they are not listed in OpenMW Lua documentation).
        if (ptr.getCellRef().getRefId() == "player")
            return "Player";
        if (isMarker(ptr))
            return "Marker";
        return getLuaObjectTypeName(static_cast<ESM::RecNameInts>(ptr.getType()), /*fallback=*/ptr.getTypeDescription());
    }

    ESM::LuaScriptCfg::Flags getLuaScriptFlag(const MWWorld::Ptr& ptr)
    {
        if (ptr.getCellRef().getRefId() == "player")
            return ESM::LuaScriptCfg::sPlayer;
        if (isMarker(ptr))
            return 0;
        auto it = luaObjectTypeInfo.find(static_cast<ESM::RecNameInts>(ptr.getType()));
        if (it != luaObjectTypeInfo.end())
            return it->second.mFlag;
        else
            return 0;
    }

    std::string ptrToString(const MWWorld::Ptr& ptr)
    {
        std::string res = "object";
        res.append(idToString(getId(ptr)));
        res.append(" (");
        res.append(getLuaObjectTypeName(ptr));
        res.append(", ");
        res.append(ptr.getCellRef().getRefId());
        res.append(")");
        return res;
    }

    std::string Object::toString() const
    {
        if (isValid())
            return ptrToString(ptr());
        else
            return "object" + idToString(mId) + " (not found)";
    }

    bool Object::isValid() const
    {
        if (mLastUpdate < mObjectRegistry->mUpdateCounter)
        {
            updatePtr();
            mLastUpdate = mObjectRegistry->mUpdateCounter;
        }
        return !mPtr.isEmpty();
    }

    const MWWorld::Ptr& Object::ptr() const
    {
        if (!isValid())
            throw std::runtime_error("Object is not available: " + idToString(mId));
        return mPtr;
    }

    void ObjectRegistry::update()
    {
        if (mChanged)
        {
            mUpdateCounter++;
            mChanged = false;
        }
    }

    void ObjectRegistry::clear()
    {
        mObjectMapping.clear();
        mChanged = false;
        mUpdateCounter = 0;
        mLastAssignedId.unset();
    }

    MWWorld::Ptr ObjectRegistry::getPtr(ObjectId id, bool local)
    {
        MWWorld::Ptr ptr;
        auto it = mObjectMapping.find(id);
        if (it != mObjectMapping.end())
            ptr = it->second;
        if (local)
        {
            // TODO: Return ptr only if it is active or was active in the previous frame, otherwise return empty.
            //     Needed because in multiplayer inactive objects will not be synchronized, so an be out of date.
        }
        else
        {
            // TODO: If Ptr is empty then try to load the object from esp/esm3.
        }
        return ptr;
    }

    ObjectId ObjectRegistry::registerPtr(const MWWorld::Ptr& ptr)
    {
        ObjectId id = ptr.getCellRef().getOrAssignRefNum(mLastAssignedId);
        mChanged = true;
        mObjectMapping[id] = ptr;
        return id;
    }

    ObjectId ObjectRegistry::deregisterPtr(const MWWorld::Ptr& ptr)
    {
        ObjectId id = getId(ptr);
        mChanged = true;
        mObjectMapping.erase(id);
        return id;
    }

}
