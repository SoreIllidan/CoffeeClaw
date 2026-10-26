// features/wallhack - ESP de dinosaurios.
//
// Lo que es especifico de esta funcionalidad: que cuenta como dinosaurio, la
// altura estimada por especie y la limpieza del nombre del Blueprint.
// El dibujo vive en main.cpp porque usa el overlay y la camara del nucleo.

#pragma once

#include <string>
#include <cstring>

namespace features {
namespace wallhack {

// Altura aproximada por especie en centimetros. Se usa cuando el bounds del
// mesh no es de fiar. Si no coincide ninguna, 200 cm.
inline double DinoHeight(const std::wstring& raw) {
    struct Entry { const wchar_t* key; double h; };
    static const Entry kTable[] = {
        { L"Dodo", 110 }, { L"Dilo", 140 }, { L"Compy", 90 }, { L"Dimorph", 120 },
        { L"Lystro", 110 }, { L"Meso", 120 }, { L"Troodon", 160 }, { L"Microraptor", 90 },
        { L"Raptor", 200 }, { L"Para", 260 }, { L"Ptera", 210 }, { L"Carno", 230 },
        { L"Trike", 230 }, { L"Stego", 260 }, { L"Anky", 200 }, { L"Sarco", 220 },
        { L"Titanoboa", 180 }, { L"Equus", 220 }, { L"Saber", 200 },
        { L"Direwolf", 180 }, { L"Argent", 320 }, { L"Quetz", 420 }, { L"Rex", 480 },
        { L"Spino", 420 }, { L"Bronto", 700 }, { L"Paracer", 620 }, { L"Mammoth", 400 },
        { L"Theri", 350 }, { L"Giga", 560 }, { L"Titan", 900 }, { L"Mosa", 500 },
        { L"Plesio", 400 }, { L"Tuso", 400 }, { L"Yuty", 350 }, { L"Allo", 300 },
        { L"Bary", 260 }, { L"Kentro", 200 }, { L"Pachy", 180 }, { L"Castor", 200 },
    };
    for (const auto& e : kTable) {
        if (raw.find(e.key) != std::wstring::npos) return e.h;
    }
    return 200.0;
}

// Dilo_Character_BP_C -> Dilo
inline std::wstring CleanDinoName(std::wstring label) {
    auto strip = [&label](const wchar_t* suffix) {
        const size_t n = wcslen(suffix);
        if (label.size() > n && label.compare(label.size() - n, n, suffix) == 0)
            label.erase(label.size() - n);
    };
    strip(L"_Character_BP_C");
    strip(L"_Character_BP");
    strip(L"_BP_C");
    if (label.rfind(L"BP_", 0) == 0) label = label.substr(3);
    return label;
}

} // namespace wallhack
} // namespace features
