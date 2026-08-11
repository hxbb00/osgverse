#include <trellis2/trellis2.h>
#include "TrellisPipeline.h"

using namespace osgVerse;
#define TP_LOG(m) OSG_NOTICE << "[TrellisPipeline] " << m;

namespace
{
    struct TrellisHandle : public osg::Referenced
    {
        trellis2_dino_model* dino;
        trellis2_ss_flow_model* ss_flow;
        trellis2_ss_dec_model* ss_dec;
        TrellisHandle() : dino(NULL), ss_flow(NULL), ss_dec(NULL) {}

        void cleanup()
        {
            if (dino) trellis2_dino_free(dino); dino = NULL;
            if (ss_flow) trellis2_ss_flow_free(ss_flow); ss_flow = NULL;
            if (ss_dec) trellis2_ss_dec_free(ss_dec); ss_dec = NULL;
        }
    };
}

TrellisPipeline::TrellisPipeline()
{ _internal = new TrellisHandle; }

TrellisPipeline::~TrellisPipeline()
{
    TrellisHandle* h = static_cast<TrellisHandle*>(_internal.get());
    if (h) h->cleanup();
}

bool TrellisPipeline::loadModelsStage1(const std::string& dinoModel, const std::string& ssFlowModel,
                                       const std::string& ssDecModel)
{
    TrellisHandle* h = static_cast<TrellisHandle*>(_internal.get());
    std::string err; if (h) h->cleanup(); else return false;

    h->dino = trellis2_dino_load(dinoModel, true, &err);
    if (!h->dino) { TP_LOG("DINO model failed to load: " << err << "\n"); return false; }
    h->ss_flow = trellis2_ss_flow_load(ssDecModel, true, &err);
    if (!h->ss_flow) { TP_LOG("SS flow model failed to load: " << err << "\n"); return false; }
    h->ss_dec = trellis2_ss_dec_load(ssDecModel, true, &err);
    if (!h->ss_dec) { TP_LOG("SS decoder model failed to load: " << err << "\n"); return false; }
    return true;
}
