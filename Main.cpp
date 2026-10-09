#include "Main.hpp"

Config &config() {
    static Config cfg;
    return cfg;
}

class PesAssetLoader {
public:
    PesAssetLoader() {
        auto v = PES::GetAppVersion();
        if (v.id() == ID_PES5 || v.id() == ID_PES6) {
            config().Read(PES::GameDirPath(L"plugins\\AssetLoader.ini"));
            Log::Init(PES::GameDirPath(L"plugins\\AssetLoader.log"), config().LOG_LEVEL > 0, true);
            InitFileLoader(v);
            InitMemoryLimits(v);
        }
    }
} pesAssetLoader;
