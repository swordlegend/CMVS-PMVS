#pragma once

#include "patch.h"
#include <list>
#include "../numeric/vec2.h"

namespace PMVS3
{

class CfindMatch;

class Cfilter
{
public:
    Cfilter(CfindMatch& findMatch);

    void init(void);
    void run(void);

    float computeGain(const Patch::Cpatch& patch);

    int filterQuad(const Patch::Cpatch& patch, const std::vector<Patch::Ppatch>& neighbors) const;

protected:
    void filterOutside(void);
    void filterOutsideThread(void);

    void filterExact(void);
    void filterExactThread(void);

    void filterNeighbor(const int time);
    void filterSmallGroups(void);
    void filterSmallGroupsSub(const int pid, const int id, std::vector<int>& label, std::list<int>& ltmp) const;
    void setDepthMaps(void);
    void setDepthMapsVGridsVPGridsAddPatchV(const int additive);

    std::vector<float> m_gains;

    std::vector<std::vector<int>> m_newimages, m_removeimages;
    std::vector<std::vector<TVec2<int>>> m_newgrids, m_removegrids;

    int m_time;
    std::vector<int> m_rejects;

    //----------------------------------------------------------------------
    // Thread related
    //----------------------------------------------------------------------
    void setDepthMapsThread(void);
    void addPatchVThread(void);
    void setVGridsVPGridsThread(void);
    void filterNeighborThread(void);

    CfindMatch& m_fm;
};

};
