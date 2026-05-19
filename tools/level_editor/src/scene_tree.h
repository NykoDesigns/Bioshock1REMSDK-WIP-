#pragma once

#include "bsm_document.h"
#include <string>

// Scene tree panel — shows all actors organized by class
class SceneTree {
public:
    void Render(BSMDocument& doc, int& selectedActor);

    // Filter
    char filterText[256] = "";
    bool showSpawners = true;
    bool showTriggers = true;
    bool showLights = true;
    bool showDoors = true;
    bool showMeshes = true;
    bool showOther = true;
};
