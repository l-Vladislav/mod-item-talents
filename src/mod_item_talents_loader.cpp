/*
 * mod-item-talents loader.
 *
 * ModulesLoader сборки вызывает Add<папка>Scripts() с заменой '-' на '_':
 * папка "mod-item-talents" -> Addmod_item_talentsScripts().
 */

// Определено в ItemTalentsScripts.cpp
void AddSC_item_talents_scripts();

void Addmod_item_talentsScripts()
{
    AddSC_item_talents_scripts();
}
