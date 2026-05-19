#pragma once

#include "bsm_document.h"

// Properties panel — shows and edits selected actor properties
class PropertiesPanel {
public:
    void Render(BSMDocument& doc, int& selectedActor);
};
