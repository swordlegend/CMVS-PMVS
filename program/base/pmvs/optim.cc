#define _USE_MATH_DEFINES
#include <math.h>

#include <algorithm>
#include <numeric>

#include "findMatch.h"
#include "optim.h"
#include <cstdio>

using namespace Patch;
using namespace PMVS3;

namespace
{

    // The base class for functors when using the (unsupported) NonLinearOptimization module
    template<typename _Scalar, int NX = Eigen::Dynamic, int NY = Eigen::Dynamic>
    struct LMFunctor
    {
        typedef _Scalar Scalar;

        enum
        {
            InputsAtCompileTime = NX,
            ValuesAtCompileTime = NY
        };

        typedef Eigen::Matrix<Scalar, InputsAtCompileTime, 1> InputType;
        typedef Eigen::Matrix<Scalar, ValuesAtCompileTime, 1> ValueType;
        typedef Eigen::Matrix<Scalar, ValuesAtCompileTime, InputsAtCompileTime> JacobianType;

        const int m_inputs, m_values;

        LMFunctor()
            : m_inputs(InputsAtCompileTime)
            , m_values(ValuesAtCompileTime)
        {}

        LMFunctor(int inputs, int values)
            : m_inputs(inputs)
            , m_values(values)
        {}

        int inputs() const { return m_inputs; }
        int values() const { return m_values; }

        // You should define that in the subclass:
        // int operator() (const InputType& x, ValueType* v, JacobianType* _j=0) const;
    };

    template<typename Scalar>
    struct LMRefinePatch : LMFunctor<Scalar>
    {
        explicit LMRefinePatch(int id)
            : LMFunctor<Scalar>(3, 3)
            , m_id(id)
        {}

        int operator()(const Eigen::Matrix<Scalar, Eigen::Dynamic, 1, 0>& x, Eigen::Matrix<Scalar, Eigen::Dynamic, 1, 0>& fvec) const
        {
            Coptim::my_f_lm(x, fvec, m_id);

            return 0;
        }

        int m_id;
    };

}

Coptim* Coptim::m_one = nullptr;

Coptim::Coptim(CfindMatch& findMatch)
    : m_fm(findMatch)
{
    m_one = this;

    m_status.resize(35);
    fill(m_status.begin(), m_status.end(), 0);  
}

void Coptim::init(void)
{
    m_vect0T.resize(m_fm.m_CPU);
    m_centersT.resize(m_fm.m_CPU);
    m_raysT.resize(m_fm.m_CPU);
    m_indexesT.resize(m_fm.m_CPU);
    m_dscalesT.resize(m_fm.m_CPU);
    m_ascalesT.resize(m_fm.m_CPU);
    m_paramsT.resize(m_fm.m_CPU);

    m_texsT.resize(m_fm.m_CPU);
    m_weightsT.resize(m_fm.m_CPU);

    for (int c = 0; c < m_fm.m_CPU; ++c)
    {
        m_texsT[c].resize(m_fm.m_num);
        m_weightsT[c].resize(m_fm.m_num);
        for (int j = 0; j < m_fm.m_tau; ++j) m_texsT[c][j].resize(3 * m_fm.m_wsize * m_fm.m_wsize);
    }

    setAxesScales();
}

void Coptim::setAxesScales(void)
{
    m_xaxes.resize(m_fm.m_num);
    m_yaxes.resize(m_fm.m_num);
    m_zaxes.resize(m_fm.m_num);
    for (int index = 0; index < m_fm.m_num; ++index)
    {
        m_zaxes[index] = Vec3f(m_fm.m_pss.m_photos[index].m_oaxis[0],
                               m_fm.m_pss.m_photos[index].m_oaxis[1],
                               m_fm.m_pss.m_photos[index].m_oaxis[2]);
        m_xaxes[index] = Vec3f(m_fm.m_pss.m_photos[index].m_projection[0][0][0],
                               m_fm.m_pss.m_photos[index].m_projection[0][0][1],
                               m_fm.m_pss.m_photos[index].m_projection[0][0][2]);
        m_yaxes[index] = cross(m_zaxes[index], m_xaxes[index]);
        unitize(m_yaxes[index]);
        m_xaxes[index] = cross(m_yaxes[index], m_zaxes[index]);
    }

    m_ipscales.resize(m_fm.m_num);
    for (int index = 0; index < m_fm.m_num; ++index)
    {
        const Vec4f xaxe(m_xaxes[index][0], m_xaxes[index][1], m_xaxes[index][2], 0.0);
        const Vec4f yaxe(m_yaxes[index][0], m_yaxes[index][1], m_yaxes[index][2], 0.0);

        const float fx = xaxe * m_fm.m_pss.m_photos[index].m_projection[0][0];
        const float fy = yaxe * m_fm.m_pss.m_photos[index].m_projection[0][1];
        m_ipscales[index] = fx + fy;
    }
}

void Coptim::collectImages(const int index, std::vector<int>& indexes) const
{
    // Find images with constraints m_angleThreshold, m_visdata, m_sequenceThreshold, m_targets. Results are sorted by CphotoSet::m_distances.
    indexes.clear();
    Vec4f ray0 = m_fm.m_pss.m_photos[index].m_oaxis;
    ray0[3] = 0.0f;

    std::vector<Vec2f> candidates;
    // Search only for related images
    for (int i = 0; i < (int)m_fm.m_visdata2[index].size(); ++i)
    {
        const int indextmp = m_fm.m_visdata2[index][i];

        if (m_fm.m_sequenceThreshold != -1 && m_fm.m_sequenceThreshold < abs(index - indextmp)) continue;

        Vec4f ray1 = m_fm.m_pss.m_photos[indextmp].m_oaxis;
        ray1[3] = 0.0f;

        if (ray0 * ray1 < cos(m_fm.m_angleThreshold0)) continue;

        candidates.push_back(Vec2f(m_fm.m_pss.m_distances[index][indextmp], indextmp));
    }

    std::sort(candidates.begin(), candidates.end(), Svec2cmp<float>());
    for (int i = 0; i < std::min(m_fm.m_tau, (int)candidates.size()); ++i) indexes.push_back((int)candidates[i][1]);
}

int Coptim::preProcess(Cpatch& patch, const int id, const int seed)
{
    addImages(patch);

    // Here define reference images, and sort images. Something similar to constraintImages is done inside.
    constraintImages(patch, m_fm.m_nccThresholdBefore, id);

    // Fix the reference image and sort the other m_tau - 1 images.
    sortImages(patch);

    // Pierre Moulon (it avoids crash in some case)
    if( (int)patch.m_images.size() > 0)
    {
        // setScales should be here to avoid noisy output
        m_fm.m_pos.setScales(patch);
    }

    // Check minimum number of images
    if ((int)patch.m_images.size() < m_fm.m_minImageNumThreshold) return 1;

    const int flag = m_fm.m_pss.checkAngles(patch.m_coord, patch.m_images,
                     m_fm.m_maxAngleThreshold,
                     m_fm.m_angleThreshold1,
                     m_fm.m_minImageNumThreshold);

    if (flag)
    {
        patch.m_images.clear();
        return 1;
    }

    return 0;
}

void Coptim::filterImagesByAngle(Cpatch& patch)
{
    std::vector<int> newindexes;

    auto bimage = patch.m_images.begin();
    auto eimage = patch.m_images.end();

    while (bimage != eimage)
    {
        const int index = *bimage;
        Vec4f ray = m_fm.m_pss.m_photos[index].m_center - patch.m_coord;
        unitize(ray);
        if (ray * patch.m_normal < cos(m_fm.m_angleThreshold1))
        {
            if (bimage == patch.m_images.begin())
            {
                patch.m_images.clear();
                return;
            }
        } else
        {
            newindexes.push_back(index);
        }
        ++bimage;
    }

    patch.m_images.swap(newindexes);
}

int Coptim::postProcess(Cpatch& patch, const int id, const int seed)
{
    if ((int)patch.m_images.size() < m_fm.m_minImageNumThreshold) return 1;
    if (m_fm.m_pss.getMask(patch.m_coord, m_fm.m_level) == 0 || m_fm.insideBimages(patch.m_coord) == 0) return 1;

    addImages(patch);

    constraintImages(patch, m_fm.m_nccThreshold, id);
    filterImagesByAngle(patch);

    if ((int)patch.m_images.size() < m_fm.m_minImageNumThreshold) return 1;

    m_fm.m_pos.setGrids(patch);

    setRefImage(patch, id);
    constraintImages(patch, m_fm.m_nccThreshold, id);

    if ((int)patch.m_images.size() < m_fm.m_minImageNumThreshold) return 1;

    m_fm.m_pos.setGrids(patch);

    // Set m_timages
    patch.m_timages = 0;
    auto begin = patch.m_images.begin();
    auto end = patch.m_images.end();
    while (begin != end)
    {
        if (*begin < m_fm.m_tnum)
        ++patch.m_timages;
        ++begin;
    }

    patch.m_tmp = patch.score2(m_fm.m_nccThreshold);

    // Set vimages vgrids.
    if (m_fm.m_depth)
    {
        m_fm.m_pos.setVImagesVGrids(patch);

        if (2 <= m_fm.m_depth && check(patch)) return 1;
    }
    return 0;
}

void Coptim::constraintImages(Cpatch& patch, const float nccThreshold, const int id)
{
    std::vector<float> inccs;
    setINCCs(patch, inccs, patch.m_images, id, 0);

    // Constraint images
    std::vector<int> newimages;
    newimages.push_back(patch.m_images[0]);
    for (int i = 1; i < (int)patch.m_images.size(); ++i)
    {
        if (inccs[i] < 1.0f - nccThreshold) newimages.push_back(patch.m_images[i]);
    }
    patch.m_images.swap(newimages);
}

void Coptim::setRefImage(Cpatch& patch, const int id)
{
    // Set the reference image only for target images
    std::vector<int> indexes;
    auto begin = patch.m_images.begin();
    auto end = patch.m_images.end();
    while (begin != end)
    {
        if (*begin < m_fm.m_tnum) indexes.push_back(*begin);
        ++begin;
    }

    // To avoid segmentation error on alley dataset. (this code is necessary because of the use of filterExact)
    if (indexes.empty())
    {
        patch.m_images.clear();
        return;
    }

    std::vector<std::vector<float> > inccs;
    setINCCs(patch, inccs, indexes, id, 1);

    int refindex = -1;
    float refncc = INT_MAX/2;
    for (int i = 0; i < (int)indexes.size(); ++i)
    {
        const float sum = std::accumulate(inccs[i].begin(), inccs[i].end(), 0.0f);
        if (sum < refncc)
        {
            refncc = sum;
            refindex = i;
        }
    }

    const int refIndex = indexes[refindex];
    for (int i = 0; i < (int)patch.m_images.size(); ++i)
    {
        if (patch.m_images[i] == refIndex)
        {
            const int itmp = patch.m_images[0];
            patch.m_images[0] = refIndex;
            patch.m_images[i] = itmp;
            break;
        }
    }
}

void Coptim::sortImages(Cpatch& patch) const
{
    const int newm = 1;
    if (newm == 1)
    {
        const float threshold = 1.0f - cos(10.0 * M_PI / 180.0);
        std::vector<int> indexes, indexes2;
        std::vector<float> units, units2;
        std::vector<Vec4f> rays, rays2;

        computeUnits(patch, indexes, units, rays);

        patch.m_images.clear();
        if (indexes.size() < 2) return;

        units[0] = 0.0f;

        while (!indexes.empty())
        {
            auto ite = min_element(units.begin(), units.end());
            const int index = ite - units.begin();

            patch.m_images.push_back(indexes[index]);

            // Remove other images within 5 degrees
            indexes2.clear();
            units2.clear();
            rays2.clear();

            for (int j = 0; j < (int)rays.size(); ++j)
            {
                if (j == index) continue;

                indexes2.push_back(indexes[j]);
                rays2.push_back(rays[j]);
                const float ftmp = std::min(threshold, std::max(threshold / 2.0f, 1.0f - rays[index] * rays[j]));

                units2.push_back(units[j] * (threshold / ftmp));
            }

            indexes2.swap(indexes);
            units2.swap(units);
            rays2.swap(rays);
        }
    } else
    {
        //Sort and grab the best m_tau images. All the other images don't matter. First image is the reference and fixed
        const float threshold = cos(5.0 * M_PI / 180.0);
        std::vector<int> indexes, indexes2;
        std::vector<float> units, units2;
        std::vector<Vec4f> rays, rays2;

        computeUnits(patch, indexes, units, rays);

        patch.m_images.clear();
        if (indexes.size() < 2) return;

        units[0] = 0.0f;

        while (!indexes.empty())
        {
            auto ite = min_element(units.begin(), units.end());
            const int index = ite - units.begin();

            patch.m_images.push_back(indexes[index]);    

            // Remove other images within 5 degrees
            indexes2.clear();    units2.clear();
            rays2.clear();
            for (int j = 0; j < (int)rays.size(); ++j)
            {
                if (rays[index] * rays[j] < threshold)
                {
                    indexes2.push_back(indexes[j]);
                    units2.push_back(units[j]);
                    rays2.push_back(rays[j]);
                }
            }
            indexes2.swap(indexes);
            units2.swap(units);
            rays2.swap(rays);
        }
    }
}

int Coptim::check(Cpatch& patch)
{
    const float gain = m_fm.m_filter.computeGain(patch, 1);
    patch.m_tmp = gain;

    if (gain < 0.0)
    {
        patch.m_images.clear();
        return 1;
    }

    std::vector<Ppatch> neighbors;
    m_fm.m_pos.findNeighbors(patch, neighbors, 1, 4, 2);

    // Only check when enough number of neighbors
    if (6 < (int)neighbors.size() && m_fm.m_filter.filterQuad(patch, neighbors))
    {
        patch.m_images.clear();
        return 1;
    }

    return 0;
}

void Coptim::removeImagesEdge(Patch::Cpatch& patch) const
{
    std::vector<int> newindexes;
    auto bimage = patch.m_images.begin();
    while (bimage != patch.m_images.end())
    {
        if (m_fm.m_pss.getEdge(patch.m_coord, *bimage, m_fm.m_level)) newindexes.push_back(*bimage);
        ++bimage;
    }

    patch.m_images.swap(newindexes);
}

void Coptim::addImages(Patch::Cpatch& patch) const
{
    // Take into account m_edge
    std::vector<int> used(m_fm.m_num, 0);

    auto bimage = patch.m_images.begin();
    auto eimage = patch.m_images.end();
    while (bimage != eimage)
    {
        used[*bimage] = 1;
        ++bimage;
    }

    bimage = m_fm.m_visdata2[patch.m_images[0]].begin();
    eimage = m_fm.m_visdata2[patch.m_images[0]].end();

    const float athreshold = cos(m_fm.m_angleThreshold0);
    while (bimage != eimage)
    {
        if (used[*bimage])
        {
            ++bimage;
            continue;
        }

        const Vec3f icoord = m_fm.m_pss.project(*bimage, patch.m_coord, m_fm.m_level);
        if (icoord[0] < 0.0f || m_fm.m_pss.getWidth(*bimage, m_fm.m_level) - 1 <= icoord[0] ||
            icoord[1] < 0.0f || m_fm.m_pss.getHeight(*bimage, m_fm.m_level) - 1 <= icoord[1])
        {
            ++bimage;
            continue;
        }

        if (m_fm.m_pss.getEdge(patch.m_coord, *bimage, m_fm.m_level) == 0)
        {
            ++bimage;
            continue;
        }

        Vec4f ray = m_fm.m_pss.m_photos[*bimage].m_center - patch.m_coord;
        unitize(ray);
        const float ftmp = ray * patch.m_normal;

        if (athreshold <= ftmp) patch.m_images.push_back(*bimage);

        ++bimage;
    }
}

void Coptim::computeUnits(const Patch::Cpatch& patch, std::vector<float>& units) const
{
    const int size = (int)patch.m_images.size();
    units.resize(size);

    auto bimage = patch.m_images.begin();
    auto bfine = units.begin();

    while (bimage != patch.m_images.end())
    {
        *bfine = INT_MAX/2;

        *bfine = getUnit(*bimage, patch.m_coord);
        Vec4f ray = m_fm.m_pss.m_photos[*bimage].m_center - patch.m_coord;
        unitize(ray);
        const float denom = ray * patch.m_normal;

        if (0.0 < denom)    *bfine /= denom;
        else                *bfine = INT_MAX/2;

        ++bimage;
        ++bfine;
    }
}

void Coptim::computeUnits(const Patch::Cpatch& patch, std::vector<int>& indexes, std::vector<float>& units, std::vector<Vec4f>& rays) const
{
    auto bimage = patch.m_images.begin();

    while (bimage != patch.m_images.end())
    {
        Vec4f ray = m_fm.m_pss.m_photos[*bimage].m_center - patch.m_coord;
        unitize(ray);
        const float dot = ray * patch.m_normal;
        if (dot <= 0.0f)
        {
            ++bimage;
            continue;
        }

        const float scale = getUnit(*bimage, patch.m_coord);
        const float fine = scale / dot;

        indexes.push_back(*bimage);
        units.push_back(fine);
        rays.push_back(ray);
        ++bimage;
    }
}

void Coptim::my_f_lm(const Eigen::VectorXd &par, Eigen::VectorXd &fvec, int id)
{
    double xs[3] = {par[0], par[1], par[2]};

    const double angle1 = xs[1] * m_one->m_ascalesT[id];
    const double angle2 = xs[2] * m_one->m_ascalesT[id];

    double ret = 0.0;

    if (angle1 <= - M_PI / 2.0 || M_PI / 2.0 <= angle1 || angle2 <= - M_PI / 2.0 || M_PI / 2.0 <= angle2)
    {
        ret = 2.0;

        fvec[0] = ret;
        fvec[1] = ret;
        fvec[2] = ret;

        return;
    }

    Vec4f coord, normal;
    m_one->decode(coord, normal, xs, id);

    const int index = m_one->m_indexesT[id][0];
    Vec4f pxaxis, pyaxis;
    m_one->getPAxes(index, coord, normal, pxaxis, pyaxis);

    const int size    = std::min(m_one->m_fm.m_tau, (int)m_one->m_indexesT[id].size());
    const int mininum = std::min(m_one->m_fm.m_minImageNumThreshold, size);

    for (int i = 0; i < size; ++i)
    {
        int flag;
        flag = m_one->grabTex(coord, pxaxis, pyaxis, normal, m_one->m_indexesT[id][i], m_one->m_fm.m_wsize, m_one->m_texsT[id][i]);

        if (flag == 0) m_one->normalize(m_one->m_texsT[id][i]);
    }

    if (m_one->m_texsT[id][0].empty()) ret = 2.0;

    double ans = 0.0;
    int denom  = 0;
    for (int i = 1; i < size; ++i)
    {
        if (m_one->m_texsT[id][i].empty()) continue;
        ans += (double)robustincc(1.0 - m_one->dot(m_one->m_texsT[id][0], m_one->m_texsT[id][i]));
        denom++;
    }

    if (denom < mininum - 1) ret = 2.0;
    else                     ret = ans / denom;

    fvec[0] = ret;
    fvec[1] = ret;
    fvec[2] = ret;
}

void Coptim::my_f_lm(const Eigen::VectorXf &par, Eigen::VectorXf &fvec, int id)
{
    double xs[3] = {par[0], par[1], par[2]};

    const double angle1 = xs[1] * m_one->m_ascalesT[id];
    const double angle2 = xs[2] * m_one->m_ascalesT[id];

    double ret = 0.0;

    if (angle1 <= - M_PI / 2.0 || M_PI / 2.0 <= angle1 || angle2 <= - M_PI / 2.0 || M_PI / 2.0 <= angle2)
    {
        ret = 2.0;

        fvec[0] = ret;
        fvec[1] = ret;
        fvec[2] = ret;

        return;
    }

    Vec4f coord, normal;
    m_one->decode(coord, normal, xs, id);

    const int index = m_one->m_indexesT[id][0];
    Vec4f pxaxis, pyaxis;
    m_one->getPAxes(index, coord, normal, pxaxis, pyaxis);

    const int size    = std::min(m_one->m_fm.m_tau, (int)m_one->m_indexesT[id].size());
    const int mininum = std::min(m_one->m_fm.m_minImageNumThreshold, size);

    for (int i = 0; i < size; ++i)
    {
        int flag;
        flag = m_one->grabTex(coord, pxaxis, pyaxis, normal, m_one->m_indexesT[id][i], m_one->m_fm.m_wsize, m_one->m_texsT[id][i]);

        if (flag == 0) m_one->normalize(m_one->m_texsT[id][i]);
    }

    if (m_one->m_texsT[id][0].empty()) ret = 2.0;

    double ans = 0.0;
    int denom  = 0;
    for (int i = 1; i < size; ++i)
    {
        if (m_one->m_texsT[id][i].empty()) continue;
        ans += (double)robustincc(1.0 - m_one->dot(m_one->m_texsT[id][0], m_one->m_texsT[id][i]));
        denom++;
    }

    if (denom < mininum - 1) ret = 2.0;
    else                     ret = ans / denom;

    fvec[0] = ret;
    fvec[1] = ret;
    fvec[2] = ret;
}

bool Coptim::refinePatchBFGS(Cpatch& patch, const int id)
{
    m_centersT[id] = patch.m_coord;
    m_raysT[id]    = patch.m_coord - m_fm.m_pss.m_photos[patch.m_images[0]].m_center;
    unitize(m_raysT[id]);
    m_indexesT[id] = patch.m_images;

    m_dscalesT[id] = patch.m_dscale;
    m_ascalesT[id] = (float)M_PI / 48.0f;

    setWeightsT(patch, id);

    double p[3];
    encode(patch.m_coord, patch.m_normal, p, id);

    Eigen::VectorXf x(3);
    x << p[0], p[1], p[2];

    LMRefinePatch<float> functor(id);
    Eigen::NumericalDiff<LMRefinePatch<float>> numDiff(functor);
    Eigen::LevenbergMarquardt<Eigen::NumericalDiff<LMRefinePatch<float>>, float> lm(numDiff);

    lm.parameters.ftol   = 1.0e-7;
    lm.parameters.xtol   = 1.0e-7;
    lm.parameters.maxfev = 100;

    auto status = lm.minimize(x);

    p[0] = x[0];
    p[1] = x[1];
    p[2] = x[2];

    // ret 0 to 3 are "good", the rest are bad
    if (status >= 0 && status <= 3)
    {
        decode(patch.m_coord, patch.m_normal, p, id);

        patch.m_ncc = 1.0f - unrobustincc(computeINCC(patch.m_coord, patch.m_normal, patch.m_images, id, 1));
    } else
    {
        return false;
    }

    return true;
}

void Coptim::encode(const Vec4f& coord, double* const vect, const int id) const
{
    vect[0] = (coord - m_centersT[id]) * m_raysT[id] / m_dscalesT[id];
}

void Coptim::encode(const Vec4f& coord, const Vec4f& normal, double* const vect, const int id) const
{
    encode(coord, vect, id);

    const int image = m_indexesT[id][0];
    const float fx = m_xaxes[image] * proj(normal); // Projects from 4D to 3D, divide by last value
    const float fy = m_yaxes[image] * proj(normal);
    const float fz = m_zaxes[image] * proj(normal);

    vect[2] = asin(std::max(-1.0f, std::min(1.0f, fy)));
    const float cosb = cos(vect[2]);

    if (cosb == 0.0f)
    {
        vect[1] = 0.0;
    } else
    {
        const float sina =  fx / cosb;
        const float cosa = -fz / cosb;
        vect[1] = acos(std::max(-1.0f, std::min(1.0f, cosa)));
        if (sina < 0.0f) vect[1] = - vect[1];
    }

    vect[1] = vect[1] / m_ascalesT[id];
    vect[2] = vect[2] / m_ascalesT[id];  
}

void Coptim::decode(Vec4f& coord, Vec4f& normal, const double* const vect, const int id) const
{
    decode(coord, vect, id);
    const int image = m_indexesT[id][0];

    const float angle1 = vect[1] * m_ascalesT[id];
    const float angle2 = vect[2] * m_ascalesT[id];

    const float fx = sin(angle1) * cos(angle2);
    const float fy = sin(angle2);
    const float fz = - cos(angle1) * cos(angle2);

    Vec3f ftmp = m_xaxes[image] * fx + m_yaxes[image] * fy + m_zaxes[image] * fz;
    normal = Vec4f(ftmp[0], ftmp[1], ftmp[2], 0.0f);
}

void Coptim::decode(Vec4f& coord, const double* const vect, const int id) const
{
    coord = m_centersT[id] + m_dscalesT[id] * (float)vect[0] * m_raysT[id];
}

void Coptim::setINCCs(const Patch::Cpatch& patch, std::vector<float> & inccs, const std::vector<int>& indexes, const int id, const int robust)
{
    const int index = indexes[0];
    Vec4f pxaxis, pyaxis;
    getPAxes(index, patch.m_coord, patch.m_normal, pxaxis, pyaxis);

    auto& texs = m_texsT[id];

    const int size = (int)indexes.size();
    for (int i = 0; i < size; ++i)
    {
        const int flag = grabTex(patch.m_coord, pxaxis, pyaxis, patch.m_normal, indexes[i], m_fm.m_wsize, texs[i]);
        if (flag == 0) normalize(texs[i]);
    }

    inccs.resize(size);

    if (texs[0].empty())
    {
        fill(inccs.begin(), inccs.end(), 2.0f);
        return;
    }

    for (int i = 0; i < size; ++i)
    {
        if (i == 0)
        {
            inccs[i] = 0.0f;
        } else if (!texs[i].empty())
        {
            if (robust == 0) inccs[i] = 1.0f - dot(texs[0], texs[i]);
            else             inccs[i] = robustincc(1.0f - dot(texs[0], texs[i]));
        } else
        {
            inccs[i] = 2.0f;
        }
    }
}

void Coptim::setINCCs(const Patch::Cpatch& patch, std::vector<std::vector<float> >& inccs, const std::vector<int>& indexes, const int id, const int robust)
{
    const int index = indexes[0];
    Vec4f pxaxis, pyaxis;
    getPAxes(index, patch.m_coord, patch.m_normal, pxaxis, pyaxis);

    auto& texs = m_texsT[id];

    const int size = (int)indexes.size();
    for (int i = 0; i < size; ++i)
    {
        const int flag = grabTex(patch.m_coord, pxaxis, pyaxis, patch.m_normal, indexes[i], m_fm.m_wsize, texs[i]);

        if (flag == 0) normalize(texs[i]);
    }

    inccs.resize(size);
    for (int i = 0; i < size; ++i)
    inccs[i].resize(size);

    for (int i = 0; i < size; ++i)
    {
        inccs[i][i] = 0.0f;
        for (int j = i+1; j < size; ++j)
        {
            if (!texs[i].empty() && !texs[j].empty())
            {
                if (robust == 0) inccs[j][i] = inccs[i][j] = 1.0f - dot(texs[i], texs[j]);
                else             inccs[j][i] = inccs[i][j] = robustincc(1.0f - dot(texs[i], texs[j]));
            } else
            {
                inccs[j][i] = inccs[i][j] = 2.0f;
            }
        }
    }
}

int Coptim::grabSafe(const int index, const int size, const Vec3f& center, const Vec3f& dx, const Vec3f& dy, const int level) const
{
    const int margin = size / 2;

    const Vec3f tl = center - dx * margin - dy * margin;
    const Vec3f tr = center + dx * margin - dy * margin;

    const Vec3f bl = center - dx * margin + dy * margin;
    const Vec3f br = center + dx * margin + dy * margin;

    const float minx = std::min(tl[0], std::min(tr[0], std::min(bl[0], br[0])));
    const float maxx = std::max(tl[0], std::max(tr[0], std::max(bl[0], br[0])));
    const float miny = std::min(tl[1], std::min(tr[1], std::min(bl[1], br[1])));
    const float maxy = std::max(tl[1], std::max(tr[1], std::max(bl[1], br[1])));

    // 1 should be enough
    const int margin2 = 3;
    // ??? may need to change if we change interpolation method
    if (minx < margin2 || m_fm.m_pss.getWidth(index, level)  - 1 - margin2 <= maxx ||
        miny < margin2 || m_fm.m_pss.getHeight(index, level) - 1 - margin2 <= maxy)
    {
        return 0;
    }

    return 1;
}

// My own optimisation
float MyPow2(int x)
{
    const float answers[] = {0.0625, 0.125, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};

    return answers[x + 4];
}

static float Log2 = log(2.0f);

int Coptim::grabTex(const Vec4f& coord, const Vec4f& pxaxis, const Vec4f& pyaxis, const Vec4f& pzaxis, const int index, const int size, std::vector<float>& tex) const
{
    tex.clear();

    Vec4f ray = m_fm.m_pss.m_photos[index].m_center - coord;
    unitize(ray);
    const float weight = std::max(0.0f, ray * pzaxis);

    //???????
    if (weight < cos(m_fm.m_angleThreshold1)) return 1;

    const int margin = size / 2;

    Vec3f center = m_fm.m_pss.project(index, coord, m_fm.m_level);
    Vec3f dx     = m_fm.m_pss.project(index, coord + pxaxis, m_fm.m_level) - center;
    Vec3f dy     = m_fm.m_pss.project(index, coord + pyaxis, m_fm.m_level) - center;

    const float ratio = (norm(dx) + norm(dy)) / 2.0f;
    int leveldif = (int)floor(log(ratio) / Log2 + 0.5f);

    // Upper limit is 2
    leveldif = std::max(-m_fm.m_level, std::min(2, leveldif));

    const float scale = MyPow2(leveldif);
    const int newlevel = m_fm.m_level + leveldif;

    center /= scale;  dx /= scale;  dy /= scale;

    if (grabSafe(index, size, center, dx, dy, newlevel) == 0) return 1;

    Vec3f left = center - dx * margin - dy * margin;

    tex.resize(3 * size * size);
    float* texp = &tex[0] - 1;
    for (int y = 0; y < size; ++y)
    {
        Vec3f vftmp = left;
        left += dy;
        for (int x = 0; x < size; ++x)
        {
            Vec3f color = m_fm.m_pss.getColor(index, vftmp[0], vftmp[1], newlevel);
            *(++texp) = color[0];
            *(++texp) = color[1];
            *(++texp) = color[2];
            vftmp += dx;
        }
    }

    return 0;
}

double Coptim::computeINCC(const Vec4f& coord, const Vec4f& normal, const std::vector<int>& indexes, const int id, const int robust)
{
    if ((int)indexes.size() < 2) return 2.0;

    const int index = indexes[0];
    Vec4f pxaxis, pyaxis;
    getPAxes(index, coord, normal, pxaxis, pyaxis);

    return computeINCC(coord, normal, indexes, pxaxis, pyaxis, id, robust);
}

double Coptim::computeINCC(const Vec4f& coord, const Vec4f& normal, const std::vector<int>& indexes, const Vec4f& pxaxis, const Vec4f& pyaxis, const int id, const int robust)
{
    if ((int)indexes.size() < 2) return 2.0;

    const int size = std::min(m_fm.m_tau, (int)indexes.size());
    auto& texs = m_texsT[id];

    for (int i = 0; i < size; ++i)
    {
        int flag;
        flag = grabTex(coord, pxaxis, pyaxis, normal,
                        indexes[i], m_fm.m_wsize, texs[i]);

        if (flag == 0) normalize(texs[i]);
    }

    if (texs[0].empty()) return 2.0;

    double score = 0.0;

    float totalweight = 0.0f;
    for (int i = 1; i < size; ++i)
    {
        if (!texs[i].empty())
        {
            totalweight += m_weightsT[id][i];
            if (robust) score += robustincc(1.0f - dot(texs[0], texs[i])) * m_weightsT[id][i];
            else        score += (1.0f - dot(texs[0], texs[i])) * m_weightsT[id][i];
        }
    }

    if (totalweight == 0.0) score = 2.0;
    else                    score /= totalweight;

    return score;
}

// Normalize only scale for each image
void Coptim::normalize(std::vector<std::vector<float>>& texs, const int size)
{
    // Compute average rgb
    Vec3f ave;
    int denom = 0;

    std::vector<Vec3f> rgbs;
    rgbs.resize(size);
    for (int i = 0; i < size; ++i)
    {
        if (texs[i].empty()) continue;

        int count = 0;
        while (count < (int)texs[i].size())
        {
            rgbs[i][0] += texs[i][count++];
            rgbs[i][1] += texs[i][count++];
            rgbs[i][2] += texs[i][count++];
        }

        rgbs[i] /= (int)texs[i].size() / 3.0f;

        ave += rgbs[i];
        ++denom;
    }

    // Overall average
    if (denom == 0) return;

    ave /= (float)denom;

    // Scale all the colors
    for (int i = 0; i < size; ++i)
    {
        if (texs[i].empty()) continue;
        int count = 0;
        // Compute scale
        Vec3f scale;
        for (int j = 0; j < 3; ++j) if (rgbs[i][j] != 0.0f) scale[j] = ave[j] / rgbs[i][j];

        while (count < (int)texs[i].size())
        {
            texs[i][count++] *= scale[0];
            texs[i][count++] *= scale[1];
            texs[i][count++] *= scale[2];
        }
    }
}

void Coptim::normalize(std::vector<float>& tex)
{
    const int size = (int)tex.size();
    const int size3 = size / 3;
    Vec3f ave;

    float* texp = &tex[0] - 1;
    for (int i = 0; i < size3; ++i)
    {
        ave[0] += *(++texp);
        ave[1] += *(++texp);
        ave[2] += *(++texp);
    }

    ave /= (float)size3;

    float ave2 = 0.0;
    texp = &tex[0] - 1;
    for (int i = 0; i < size3; ++i)
    {
        const float f0 = ave[0] - *(++texp);
        const float f1 = ave[1] - *(++texp);
        const float f2 = ave[2] - *(++texp);

        ave2 += f0 * f0 + f1 * f1 + f2 * f2;
    }

    ave2 = sqrt(ave2 / size);

    if (ave2 == 0.0f) ave2 = 1.0f;

    texp = &tex[0] - 1;
    for (int i = 0; i < size3; ++i)
    {
        *(++texp) -= ave[0];    *texp /= ave2;
        *(++texp) -= ave[1];    *texp /= ave2;
        *(++texp) -= ave[2];    *texp /= ave2;
    }
}

float Coptim::dot(const std::vector<float>& tex0, const std::vector<float>& tex1) const
{
#ifndef PMVS_WNCC
    // Pierre Moulon (use classic access to array, windows STL do not like begin()-1)
    const int size = (int)tex0.size();
    float ans = 0.0f;
    for (int i = 0; i < size; ++i) ans += tex0[i] * tex1[i];

    return ans / size;
#else
    const int size = (int)tex0.size();
    auto i0 = tex0.begin();
    auto i1 = tex1.begin();
    float ans = 0.0f;
    for (int i = 0; i < size; ++i, ++i0, ++i1) ans += (*i0) * (*i1) * m_template[i];

    return ans;
#endif
}

float Coptim::getUnit(const int index, const Vec4f& coord) const
{
    const float fz = norm(coord - m_fm.m_pss.m_photos[index].m_center);
    const float ftmp = m_ipscales[index];
    if (ftmp == 0.0) return 1.0;

    return 2.0f * fz * (0x0001 << m_fm.m_level) / ftmp;
}

// Get x and y axis to collect textures given reference image and normal
void Coptim::getPAxes(const int index, const Vec4f& coord, const Vec4f& normal, Vec4f& pxaxis, Vec4f& pyaxis) const
{
    // Yasu changed here for fpmvs
    const float pscale = getUnit(index, coord);

    Vec3f normal3(normal[0], normal[1], normal[2]);
    Vec3f yaxis3 = cross(normal3, m_xaxes[index]);
    unitize(yaxis3);
    Vec3f xaxis3 = cross(yaxis3, normal3);
    pxaxis[0] = xaxis3[0];  pxaxis[1] = xaxis3[1];  pxaxis[2] = xaxis3[2];  pxaxis[3] = 0.0;
    pyaxis[0] = yaxis3[0];  pyaxis[1] = yaxis3[1];  pyaxis[2] = yaxis3[2];  pyaxis[3] = 0.0;

    pxaxis *= pscale;
    pyaxis *= pscale;
    const float xdis = norm(m_fm.m_pss.project(index, coord + pxaxis, m_fm.m_level) - m_fm.m_pss.project(index, coord, m_fm.m_level));
    const float ydis = norm(m_fm.m_pss.project(index, coord + pyaxis, m_fm.m_level) - m_fm.m_pss.project(index, coord, m_fm.m_level));
    pxaxis /= xdis;
    pyaxis /= ydis;
}

void Coptim::setWeightsT(const Patch::Cpatch& patch, const int id)
{
    computeUnits(patch, m_weightsT[id]);
    for (auto& weight : m_weightsT[id]) weight = std::min(1.0f, m_weightsT[id][0] / weight);  
    m_weightsT[id][0] = 1.0f;
}
