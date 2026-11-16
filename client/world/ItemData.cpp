#include "client/world/ItemData.h"

#include "shared/core/Log.h"
#include "shared/core/Paths.h"

namespace vo::client::world {

void ItemData::load()
{
    std::string error;
    if (const auto file = game::locateItemsFile())
    {
        catalogLoaded = game::loadItemCatalog(*file, catalog, error);
        if (catalogLoaded)
            VO_LOG_INFO("items: {} template(s) from {}", catalog.size(), core::pathToUtf8(*file));
        else
            VO_LOG_ERROR("items: {} (items are shown as '?', no weapons are drawn)", error);
    }
    else
    {
        VO_LOG_ERROR("items: {} not found (items are shown as '?', no weapons are drawn)", game::kItemsFile);
    }
    error.clear();
    if (const auto file = game::locateAttachmentsFile())
    {
        attachmentsLoaded = game::loadAttachmentTable(*file, attachments, error);
        if (attachmentsLoaded)
            VO_LOG_INFO("items: {} weapon attachment(s) from {}", attachments.size(), core::pathToUtf8(*file));
        else
            VO_LOG_ERROR("items: {} (no weapons are drawn)", error);
    }
    else
    {
        VO_LOG_ERROR("items: {} not found (no weapons are drawn)", game::kAttachmentsFile);
    }
    error.clear();
    if (const auto file = game::locateMonstersFile())
    {
        monstersLoaded = game::loadMonsterData(*file, monsters, error);
        if (monstersLoaded)
            VO_LOG_INFO("monsters: {} template(s), {} safe zone(s) from {}", monsters.templates().size(),
                        monsters.safeZones().size(), core::pathToUtf8(*file));
        else
            VO_LOG_ERROR("monsters: {} (monsters are drawn without a model)", error);
    }
    else
    {
        VO_LOG_ERROR("monsters: {} not found (monsters are drawn without a model)", game::kMonstersFile);
    }
}

} // namespace vo::client::world
