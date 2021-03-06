package org.dolphinemu.dolphinemu.features.settings.model.view;

import org.dolphinemu.dolphinemu.features.settings.ui.MenuTag;

public final class SubmenuSetting extends SettingsItem
{
  private MenuTag mMenuKey;

  public SubmenuSetting(String key, int titleId, MenuTag menuKey)
  {
    super(null, null, key, titleId, 0);
    mMenuKey = menuKey;
  }

  public MenuTag getMenuKey()
  {
    return mMenuKey;
  }

  @Override
  public int getType()
  {
    return TYPE_SUBMENU;
  }
}
