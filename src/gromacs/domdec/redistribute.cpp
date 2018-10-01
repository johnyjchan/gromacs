/*
 * This file is part of the GROMACS molecular simulation package.
 *
 * Copyright (c) 2005,2006,2007,2008,2009,2010,2011,2012,2013,2014,2015,2016,2017,2018, by the GROMACS development team, led by
 * Mark Abraham, David van der Spoel, Berk Hess, and Erik Lindahl,
 * and including many others, as listed in the AUTHORS file in the
 * top-level source directory and at http://www.gromacs.org.
 *
 * GROMACS is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * GROMACS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GROMACS; if not, see
 * http://www.gnu.org/licenses, or write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA.
 *
 * If you want to redistribute modifications to GROMACS, please
 * consider that scientific software is very special. Version
 * control is crucial - bugs must be traceable. We will be happy to
 * consider code for inclusion in the official distribution, but
 * derived work must not be called official GROMACS. Details are found
 * in the README & COPYING files - if they are missing, get the
 * official version at http://www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the research papers on the package. Check out http://www.gromacs.org.
 */
/* \internal \file
 *
 * \brief Implements atom redistribution functions.
 *
 * \author Berk Hess <hess@kth.se>
 * \ingroup module_domdec
 */

#include "gmxpre.h"

#include "redistribute.h"

#include <cstring>

#include "gromacs/domdec/domdec_network.h"
#include "gromacs/domdec/ga2la.h"
#include "gromacs/gmxlib/nrnb.h"
#include "gromacs/math/vec.h"
#include "gromacs/mdlib/gmx_omp_nthreads.h"
#include "gromacs/mdlib/nsgrid.h"
#include "gromacs/mdtypes/forcerec.h"
#include "gromacs/mdtypes/nblist.h"
#include "gromacs/mdtypes/state.h"
#include "gromacs/utility/cstringutil.h"
#include "gromacs/utility/fatalerror.h"
#include "gromacs/utility/gmxomp.h"

#include "domdec_internal.h"
#include "utility.h"

/* The size per charge group of the cggl_flag buffer in gmx_domdec_comm_t */
static constexpr int DD_CGIBS = 2;

/* The flags for the cggl_flag buffer in gmx_domdec_comm_t */

/* The lower 16 bits are reserved for the charge group size */
static constexpr int DD_FLAG_NRCG = 65535;

/* Returns which bit tells whether to move a group forward along dimension d */
static inline int DD_FLAG_FW(int d)
{
    return 1 << (16 + d*2);
}

/* Returns which bit tells whether to move a group backward along dimension d */
static inline int DD_FLAG_BW(int d)
{
    return 1 << (16 + d*2 + 1);
}

static void
copyMovedAtomsToBufferPerAtom(gmx::ArrayRef<const int> move,
                              const gmx::RangePartitioning &atomGroups,
                              int nvec, int vec,
                              rvec *src, gmx_domdec_comm_t *comm)
{
    int pos_vec[DIM*2] = { 0 };

    for (int g = 0; g < move.size(); g++)
    {
        const auto atomGroup = atomGroups.block(g);
        /* Skip moved atoms */
        int        m         = move[g];
        if (m >= 0)
        {
            /* Copy to the communication buffer */
            int numAtoms  = atomGroup.size();
            pos_vec[m]   += 1 + vec*numAtoms;
            for (int i : atomGroup)
            {
                copy_rvec(src[i], comm->cgcm_state[m][pos_vec[m]++]);
            }
            pos_vec[m] += (nvec - vec - 1)*numAtoms;
        }
    }
}

static void
copyMovedAtomsToBufferPerChargeGroup(gmx::ArrayRef<const int> move,
                                     const gmx::RangePartitioning &atomGroups,
                                     int nvec, rvec *src, gmx_domdec_comm_t *comm)
{
    int pos_vec[DIM*2] = { 0 };

    for (int g = 0; g < move.size(); g++)
    {
        const auto atomGroup = atomGroups.block(g);
        /* Skip moved atoms */
        int        m         = move[g];
        if (m >= 0)
        {
            /* Copy to the communication buffer */
            copy_rvec(src[g], comm->cgcm_state[m][pos_vec[m]]);
            pos_vec[m] += 1 + atomGroup.size()*nvec;
        }
    }
}

static void clear_and_mark_ind(gmx::ArrayRef<const int>      move,
                               gmx::ArrayRef<const int>      globalAtomGroupIndices,
                               const gmx::RangePartitioning &atomGroups,
                               gmx::ArrayRef<const int>      globalAtomIndices,
                               gmx_ga2la_t                  *ga2la,
                               char                         *bLocalCG,
                               int                          *cell_index)
{
    for (int g = 0; g < move.size(); g++)
    {
        if (move[g] >= 0)
        {
            /* Clear the global indices */
            for (int a : atomGroups.block(g))
            {
                ga2la->erase(globalAtomIndices[a]);
            }
            if (bLocalCG)
            {
                bLocalCG[globalAtomGroupIndices[g]] = FALSE;
            }
            /* Signal that this group has moved using the ns cell index.
             * Here we set it to -1. fill_grid will change it
             * from -1 to NSGRID_SIGNAL_MOVED_FAC*grid->ncells.
             */
            cell_index[g] = -1;
        }
    }
}

static void print_cg_move(FILE *fplog,
                          gmx_domdec_t *dd,
                          int64_t step, int cg, int dim, int dir,
                          gmx_bool bHaveCgcmOld, real limitd,
                          rvec cm_old, rvec cm_new, real pos_d)
{
    gmx_domdec_comm_t *comm;
    char               buf[22];

    comm = dd->comm;

    fprintf(fplog, "\nStep %s:\n", gmx_step_str(step, buf));
    if (limitd > 0)
    {
        fprintf(fplog, "%s %d moved more than the distance allowed by the domain decomposition (%f) in direction %c\n",
                dd->comm->bCGs ? "The charge group starting at atom" : "Atom",
                ddglatnr(dd, dd->atomGrouping().block(cg).begin()), limitd, dim2char(dim));
    }
    else
    {
        /* We don't have a limiting distance available: don't print it */
        fprintf(fplog, "%s %d moved more than the distance allowed by the domain decomposition in direction %c\n",
                dd->comm->bCGs ? "The charge group starting at atom" : "Atom",
                ddglatnr(dd, dd->atomGrouping().block(cg).begin()), dim2char(dim));
    }
    fprintf(fplog, "distance out of cell %f\n",
            dir == 1 ? pos_d - comm->cell_x1[dim] : pos_d - comm->cell_x0[dim]);
    if (bHaveCgcmOld)
    {
        fprintf(fplog, "Old coordinates: %8.3f %8.3f %8.3f\n",
                cm_old[XX], cm_old[YY], cm_old[ZZ]);
    }
    fprintf(fplog, "New coordinates: %8.3f %8.3f %8.3f\n",
            cm_new[XX], cm_new[YY], cm_new[ZZ]);
    fprintf(fplog, "Old cell boundaries in direction %c: %8.3f %8.3f\n",
            dim2char(dim),
            comm->old_cell_x0[dim], comm->old_cell_x1[dim]);
    fprintf(fplog, "New cell boundaries in direction %c: %8.3f %8.3f\n",
            dim2char(dim),
            comm->cell_x0[dim], comm->cell_x1[dim]);
}

[[ noreturn ]]
static void cg_move_error(FILE *fplog,
                          gmx_domdec_t *dd,
                          int64_t step, int cg, int dim, int dir,
                          gmx_bool bHaveCgcmOld, real limitd,
                          rvec cm_old, rvec cm_new, real pos_d)
{
    if (fplog)
    {
        print_cg_move(fplog, dd, step, cg, dim, dir,
                      bHaveCgcmOld, limitd, cm_old, cm_new, pos_d);
    }
    print_cg_move(stderr, dd, step, cg, dim, dir,
                  bHaveCgcmOld, limitd, cm_old, cm_new, pos_d);
    gmx_fatal(FARGS,
              "%s moved too far between two domain decomposition steps\n"
              "This usually means that your system is not well equilibrated",
              dd->comm->bCGs ? "An atom group" : "An atom");
}

static void rotate_state_atom(t_state *state, int a)
{
    if (state->flags & (1 << estX))
    {
        /* Rotate the complete state; for a rectangular box only */
        state->x[a][YY] = state->box[YY][YY] - state->x[a][YY];
        state->x[a][ZZ] = state->box[ZZ][ZZ] - state->x[a][ZZ];
    }
    if (state->flags & (1 << estV))
    {
        state->v[a][YY] = -state->v[a][YY];
        state->v[a][ZZ] = -state->v[a][ZZ];
    }
    if (state->flags & (1 << estCGP))
    {
        state->cg_p[a][YY] = -state->cg_p[a][YY];
        state->cg_p[a][ZZ] = -state->cg_p[a][ZZ];
    }
}

/* Returns a pointer to a buffer of size \p numAtomsNew with contents 0 from numAtomsOld upwards
 *
 * Note: numAtomsOld should either be 0 or match the current buffer size.
 */
static int *getMovedBuffer(gmx_domdec_comm_t *comm,
                           size_t             numAtomsOld,
                           size_t             numAtomsNew)
{
    std::vector<int> &movedBuffer = comm->movedBuffer;

    GMX_RELEASE_ASSERT(numAtomsOld == 0 || movedBuffer.size() == numAtomsOld, "numAtomsOld should either be 0 or match the current size");

    /* Contents up to numAtomsOld should be preserved, clear from numAtomsOld */
    if (numAtomsOld == 0)
    {
        movedBuffer.clear();
    }
    movedBuffer.resize(numAtomsNew);

    return movedBuffer.data();
}

/* Bounds to determine whether an atom group moved to far between DD steps */
struct MoveLimits
{
    rvec distance; /* Maximum distance over which a group can move */
    rvec lower;    /* Lower bound for group location */
    rvec upper;    /* Upper bound for group location */
};

/* Compute flag that tells where to move an atom group
 *
 * The input dev tells whether the group moves fw,remains,bw (-1,0,1) along
 * dimensions.
 * The return value is -1 when the group does not move.
 * The return has move flags set when the group does move and the lower 4 bits
 * are (mis)used to tell which is the first dimension (bit 1,2,3) the group
 * needs to be moved along and in which direction (bit 0 not set for fw
 * and set for bw).
 */
static int computeMoveFlag(const gmx_domdec_t &dd,
                           const ivec         &dev)
{
    int flag              = 0;
    int firstMoveDimValue = -1;
    for (int d = 0; d < dd.ndim; d++)
    {
        const int dim = dd.dim[d];
        if (dev[dim] == 1)
        {
            flag |= DD_FLAG_FW(d);
            if (firstMoveDimValue == -1)
            {
                firstMoveDimValue = d*2;
            }
        }
        else if (dev[dim] == -1)
        {
            flag |= DD_FLAG_BW(d);
            if (firstMoveDimValue == -1)
            {
                if (dd.nc[dim] > 2)
                {
                    firstMoveDimValue = d*2 + 1;
                }
                else
                {
                    firstMoveDimValue = d*2;
                }
            }
        }
    }

    return firstMoveDimValue + flag;
}

/* Determine to which domains atomGroups in the range \p cg_start, \p cg_end should go.
 *
 * Returns in the move array where the groups should go.
 * Also updates \p cg_cm for jumps over periodic boundaries.
 *
 * \TODO Rename cg to atomGroup.
 */
static void calc_cg_move(FILE *fplog, int64_t step,
                         gmx_domdec_t *dd,
                         t_state *state,
                         const ivec tric_dir, matrix tcm,
                         const rvec cell_x0, const rvec cell_x1,
                         const MoveLimits &moveLimits,
                         const gmx::RangePartitioning &atomGroups,
                         int cg_start, int cg_end,
                         rvec *cg_cm,
                         gmx::ArrayRef<int> move)
{
    const int npbcdim = dd->npbcdim;

    for (int g = cg_start; g < cg_end; g++)
    {
        const auto atomGroup = atomGroups.block(g);
        const int  numAtoms  = atomGroup.size();
        // TODO: Rename this center of geometry variable to cogNew
        rvec       cm_new;
        if (numAtoms == 1)
        {
            copy_rvec(state->x[atomGroup.begin()], cm_new);
        }
        else
        {
            real invNumAtoms = 1/static_cast<real>(numAtoms);
            clear_rvec(cm_new);
            for (int k : atomGroup)
            {
                rvec_inc(cm_new, state->x[k]);
            }
            for (int d = 0; d < DIM; d++)
            {
                cm_new[d] = invNumAtoms*cm_new[d];
            }
        }

        ivec dev = { 0 };
        /* Do pbc and check DD cell boundary crossings */
        for (int d = DIM - 1; d >= 0; d--)
        {
            if (dd->nc[d] > 1)
            {
                bool bScrew = (dd->bScrewPBC && d == XX);
                /* Determine the location of this cg in lattice coordinates */
                real pos_d = cm_new[d];
                if (tric_dir[d])
                {
                    for (int d2 = d + 1; d2 < DIM; d2++)
                    {
                        pos_d += cm_new[d2]*tcm[d2][d];
                    }
                }
                /* Put the charge group in the triclinic unit-cell */
                if (pos_d >= cell_x1[d])
                {
                    if (pos_d >= moveLimits.upper[d])
                    {
                        cg_move_error(fplog, dd, step, g, d, 1,
                                      cg_cm != as_rvec_array(state->x.data()), moveLimits.distance[d],
                                      cg_cm[g], cm_new, pos_d);
                    }
                    dev[d] = 1;
                    if (dd->ci[d] == dd->nc[d] - 1)
                    {
                        rvec_dec(cm_new, state->box[d]);
                        if (bScrew)
                        {
                            cm_new[YY] = state->box[YY][YY] - cm_new[YY];
                            cm_new[ZZ] = state->box[ZZ][ZZ] - cm_new[ZZ];
                        }
                        for (int k : atomGroup)
                        {
                            rvec_dec(state->x[k], state->box[d]);
                            if (bScrew)
                            {
                                rotate_state_atom(state, k);
                            }
                        }
                    }
                }
                else if (pos_d < cell_x0[d])
                {
                    if (pos_d < moveLimits.lower[d])
                    {
                        cg_move_error(fplog, dd, step, g, d, -1,
                                      cg_cm != as_rvec_array(state->x.data()), moveLimits.distance[d],
                                      cg_cm[g], cm_new, pos_d);
                    }
                    dev[d] = -1;
                    if (dd->ci[d] == 0)
                    {
                        rvec_inc(cm_new, state->box[d]);
                        if (bScrew)
                        {
                            cm_new[YY] = state->box[YY][YY] - cm_new[YY];
                            cm_new[ZZ] = state->box[ZZ][ZZ] - cm_new[ZZ];
                        }
                        for (int k : atomGroup)
                        {
                            rvec_inc(state->x[k], state->box[d]);
                            if (bScrew)
                            {
                                rotate_state_atom(state, k);
                            }
                        }
                    }
                }
            }
            else if (d < npbcdim)
            {
                /* Put the charge group in the rectangular unit-cell */
                while (cm_new[d] >= state->box[d][d])
                {
                    rvec_dec(cm_new, state->box[d]);
                    for (int k : atomGroup)
                    {
                        rvec_dec(state->x[k], state->box[d]);
                    }
                }
                while (cm_new[d] < 0)
                {
                    rvec_inc(cm_new, state->box[d]);
                    for (int k : atomGroup)
                    {
                        rvec_inc(state->x[k], state->box[d]);
                    }
                }
            }
        }

        copy_rvec(cm_new, cg_cm[g]);

        /* Temporarily store the flag in move */
        move[g] = computeMoveFlag(*dd, dev);
    }
}

struct PbcAndFlag
{
    /* Constructor that purposely does not initialize anything */
    PbcAndFlag()
    {
    }

    gmx::RVec pbcShift;
    int       moveFlag;
};

/* Determine to which domains update groups in the range \p groupBegin, \p groupEnd should go.
 *
 * Returns in the move array where the groups should go.
 * Also updates the COGs and coordinates for jumps over periodic boundaries.
 */
static void calcGroupMove(FILE *fplog, int64_t step,
                          gmx_domdec_t *dd,
                          t_state *state,
                          const ivec tric_dir, matrix tcm,
                          const rvec cell_x0, const rvec cell_x1,
                          const MoveLimits &moveLimits,
                          int groupBegin, int groupEnd,
                          gmx::ArrayRef<PbcAndFlag> pbcAndFlags)
{
    GMX_RELEASE_ASSERT(!dd->bScrewPBC, "Screw PBC is not supported here");

    const int             npbcdim         = dd->npbcdim;

    gmx::UpdateGroupsCog *updateGroupsCog = dd->comm->updateGroupsCog.get();

    for (int g = groupBegin; g < groupEnd; g++)
    {

        gmx::RVec &cog    = updateGroupsCog->cog(g);
        gmx::RVec  cogOld = cog;

        ivec       dev = { 0 };
        /* Do pbc and check DD cell boundary crossings */
        for (int d = DIM - 1; d >= 0; d--)
        {
            if (dd->nc[d] > 1)
            {
                /* Determine the location of this COG in lattice coordinates */
                real pos_d = cog[d];
                if (tric_dir[d])
                {
                    for (int d2 = d + 1; d2 < DIM; d2++)
                    {
                        pos_d += cog[d2]*tcm[d2][d];
                    }
                }
                /* Put the COG in the triclinic unit-cell */
                if (pos_d >= cell_x1[d])
                {
                    if (pos_d >= moveLimits.upper[d])
                    {
                        cg_move_error(fplog, dd, step, g, d, 1,
                                      true, moveLimits.distance[d],
                                      cogOld, cog, pos_d);
                    }
                    dev[d] = 1;
                    if (dd->ci[d] == dd->nc[d] - 1)
                    {
                        rvec_dec(cog, state->box[d]);
                    }
                }
                else if (pos_d < cell_x0[d])
                {
                    if (pos_d < moveLimits.lower[d])
                    {
                        cg_move_error(fplog, dd, step, g, d, -1,
                                      true, moveLimits.distance[d],
                                      cogOld, cog, pos_d);
                    }
                    dev[d] = -1;
                    if (dd->ci[d] == 0)
                    {
                        rvec_inc(cog, state->box[d]);
                    }
                }
            }
            else if (d < npbcdim)
            {
                /* Put the COG in the rectangular unit-cell */
                while (cog[d] >= state->box[d][d])
                {
                    rvec_dec(cog, state->box[d]);
                }
                while (cog[d] < 0)
                {
                    rvec_inc(cog, state->box[d]);
                }
            }
        }

        /* Store the PBC and move flag, so we can later apply them to the atoms */
        PbcAndFlag &pbcAndFlag = pbcAndFlags[g];

        rvec_sub(cog, cogOld, pbcAndFlag.pbcShift);
        pbcAndFlag.moveFlag = computeMoveFlag(*dd, dev);
    }
}

static void
applyPbcAndSetMoveFlags(const gmx::UpdateGroupsCog      &updateGroupsCog,
                        gmx::ArrayRef<const PbcAndFlag>  pbcAndFlags,
                        int                              atomBegin,
                        int                              atomEnd,
                        gmx::ArrayRef<gmx::RVec>         atomCoords,
                        gmx::ArrayRef<int>               move)
{
    for (int a = atomBegin; a < atomEnd; a++)
    {
        const PbcAndFlag &pbcAndFlag = pbcAndFlags[updateGroupsCog.cogIndex(a)];
        rvec_inc(atomCoords[a], pbcAndFlag.pbcShift);
        /* Temporarily store the flag in move */
        move[a] = pbcAndFlag.moveFlag;
    }
}

void dd_redistribute_cg(FILE *fplog, int64_t step,
                        gmx_domdec_t *dd, ivec tric_dir,
                        t_state *state, PaddedRVecVector *f,
                        t_forcerec *fr,
                        t_nrnb *nrnb,
                        int *ncg_moved)
{
    gmx_domdec_comm_t *comm = dd->comm;

    if (dd->bScrewPBC)
    {
        check_screw_box(state->box);
    }

    rvec *cg_cm = nullptr;
    if (fr->cutoff_scheme == ecutsGROUP)
    {
        cg_cm = fr->cg_cm;
    }

    // Positions are always present, so there's nothing to flag
    bool                bV   = (state->flags & (1<<estV)) != 0;
    bool                bCGP = (state->flags & (1<<estCGP)) != 0;

    DDBufferAccess<int> moveBuffer(comm->intBuffer, dd->ncg_home);
    gmx::ArrayRef<int>  move = moveBuffer.buffer;

    const int           npbcdim = dd->npbcdim;

    rvec                cell_x0, cell_x1;
    MoveLimits          moveLimits;
    for (int d = 0; (d < DIM); d++)
    {
        moveLimits.distance[d] = dd->comm->cellsize_min[d];
        if (d >= npbcdim && dd->ci[d] == 0)
        {
            cell_x0[d] = -GMX_FLOAT_MAX;
        }
        else
        {
            cell_x0[d] = comm->cell_x0[d];
        }
        if (d >= npbcdim && dd->ci[d] == dd->nc[d] - 1)
        {
            cell_x1[d] = GMX_FLOAT_MAX;
        }
        else
        {
            cell_x1[d] = comm->cell_x1[d];
        }
        if (d < npbcdim)
        {
            moveLimits.lower[d] = comm->old_cell_x0[d] - moveLimits.distance[d];
            moveLimits.upper[d] = comm->old_cell_x1[d] + moveLimits.distance[d];
        }
        else
        {
            /* We check after communication if a charge group moved
             * more than one cell. Set the pre-comm check limit to float_max.
             */
            moveLimits.lower[d] = -GMX_FLOAT_MAX;
            moveLimits.upper[d] =  GMX_FLOAT_MAX;
        }
    }

    matrix tcm;
    make_tric_corr_matrix(npbcdim, state->box, tcm);

    const gmx::RangePartitioning &atomGrouping = dd->atomGrouping();

    const int                     nthread = gmx_omp_nthreads_get(emntDomdec);

    /* Compute the center of geometry for all home charge groups
     * and put them in the box and determine where they should go.
     */
    std::vector<PbcAndFlag>  pbcAndFlags(comm->useUpdateGroups ? comm->updateGroupsCog->numCogs() : 0);

#pragma omp parallel num_threads(nthread)
    {
        try
        {
            const int thread = gmx_omp_get_thread_num();

            if (comm->useUpdateGroups)
            {
                const auto &updateGroupsCog = *comm->updateGroupsCog;
                const int   numGroups       = updateGroupsCog.numCogs();
                calcGroupMove(fplog, step, dd, state, tric_dir, tcm,
                              cell_x0, cell_x1, moveLimits,
                              ( thread   *numGroups)/nthread,
                              ((thread+1)*numGroups)/nthread,
                              pbcAndFlags);
                /* We need a barrier as atoms below can be in a COG of a different thread */
#pragma omp barrier
                const int numHomeAtoms = comm->atomRanges.numHomeAtoms();
                applyPbcAndSetMoveFlags(updateGroupsCog, pbcAndFlags,
                                        ( thread   *numHomeAtoms)/nthread,
                                        ((thread+1)*numHomeAtoms)/nthread,
                                        state->x,
                                        move);
            }
            else
            {
                /* Here we handle single atoms or charge groups */
                calc_cg_move(fplog, step, dd, state, tric_dir, tcm,
                             cell_x0, cell_x1, moveLimits,
                             atomGrouping,
                             ( thread   *dd->ncg_home)/nthread,
                             ((thread+1)*dd->ncg_home)/nthread,
                             fr->cutoff_scheme == ecutsGROUP ? cg_cm : as_rvec_array(state->x.data()),
                             move);
            }
        }
        GMX_CATCH_ALL_AND_EXIT_WITH_FATAL_ERROR;
    }

    int ncg[DIM*2] = { 0 };
    int nat[DIM*2] = { 0 };
    for (int cg = 0; cg < dd->ncg_home; cg++)
    {
        if (move[cg] >= 0)
        {
            const int flag = move[cg] & ~DD_FLAG_NRCG;
            const int mc   = move[cg] & DD_FLAG_NRCG;
            move[cg]       = mc;

            std::vector<int> &cggl_flag = comm->cggl_flag[mc];

            /* TODO: See if we can use push_back instead */
            if ((ncg[mc] + 1)*DD_CGIBS > gmx::index(cggl_flag.size()))
            {
                cggl_flag.resize((ncg[mc] + 1)*DD_CGIBS);
            }
            cggl_flag[ncg[mc]*DD_CGIBS  ] = dd->globalAtomGroupIndices[cg];
            /* We store the cg size in the lower 16 bits
             * and the place where the charge group should go
             * in the next 6 bits. This saves some communication volume.
             */
            const int nrcg = atomGrouping.block(cg).size();
            cggl_flag[ncg[mc]*DD_CGIBS+1] = nrcg | flag;
            ncg[mc] += 1;
            nat[mc] += nrcg;
        }
    }

    inc_nrnb(nrnb, eNR_CGCM, comm->atomRanges.numHomeAtoms());
    inc_nrnb(nrnb, eNR_RESETX, dd->ncg_home);

    *ncg_moved = 0;
    for (int i = 0; i < dd->ndim*2; i++)
    {
        *ncg_moved += ncg[i];
    }

    int nvec = 1;
    if (bV)
    {
        nvec++;
    }
    if (bCGP)
    {
        nvec++;
    }

    /* Make sure the communication buffers are large enough */
    for (int mc = 0; mc < dd->ndim*2; mc++)
    {
        size_t nvr = ncg[mc] + nat[mc]*nvec;
        if (nvr > comm->cgcm_state[mc].size())
        {
            comm->cgcm_state[mc].resize(nvr);
        }
    }

    switch (fr->cutoff_scheme)
    {
        case ecutsGROUP:
            /* Recalculating cg_cm might be cheaper than communicating,
             * but that could give rise to rounding issues.
             */
            copyMovedAtomsToBufferPerChargeGroup(move, dd->atomGrouping(),
                                                 nvec, cg_cm, comm);
            break;
        case ecutsVERLET:
            /* Without charge groups we send the moved atom coordinates
             * over twice. This is so the code below can be used without
             * many conditionals for both for with and without charge groups.
             */
            copyMovedAtomsToBufferPerChargeGroup(move, dd->atomGrouping(),
                                                 nvec, as_rvec_array(state->x.data()), comm);
            break;
        default:
            gmx_incons("unimplemented");
    }

    int vectorIndex = 0;
    copyMovedAtomsToBufferPerAtom(move, dd->atomGrouping(),
                                  nvec, vectorIndex++,
                                  as_rvec_array(state->x.data()),
                                  comm);
    if (bV)
    {
        copyMovedAtomsToBufferPerAtom(move, dd->atomGrouping(),
                                      nvec, vectorIndex++,
                                      as_rvec_array(state->v.data()),
                                      comm);
    }
    if (bCGP)
    {
        copyMovedAtomsToBufferPerAtom(move, dd->atomGrouping(),
                                      nvec, vectorIndex++,
                                      as_rvec_array(state->cg_p.data()),
                                      comm);
    }

    int *moved;
    if (fr->cutoff_scheme == ecutsVERLET)
    {
        moved = getMovedBuffer(comm, 0, dd->ncg_home);
    }
    else
    {
        moved = fr->ns->grid->cell_index;
    }

    clear_and_mark_ind(move,
                       dd->globalAtomGroupIndices, dd->atomGrouping(), dd->globalAtomIndices,
                       dd->ga2la, comm->bLocalCG,
                       moved);

    /* Now we can remove the excess global atom-group indices from the list */
    dd->globalAtomGroupIndices.resize(dd->ncg_home);
    dd->atomGrouping_.reduceNumBlocks(dd->ncg_home);

    /* We reuse the intBuffer without reacquiring since we are in the same scope */
    DDBufferAccess<int> &flagBuffer = moveBuffer;

    const cginfo_mb_t   *cginfo_mb  = fr->cginfo_mb;

    /* Temporarily store atoms passed to our rank at the end of the range */
    int home_pos_cg = dd->ncg_home;
    int home_pos_at = dd->atomGrouping().subRange(0, dd->ncg_home).end();
    for (int d = 0; d < dd->ndim; d++)
    {
        DDBufferAccess<gmx::RVec> rvecBuffer(comm->rvecBuffer, 0);

        const int                 dim = dd->dim[d];
        int ncg_recv                  = 0;
        int                       nvr = 0;
        for (int dir = 0; dir < (dd->nc[dim] == 2 ? 1 : 2); dir++)
        {
            const int cdd = d*2 + dir;
            /* Communicate the cg and atom counts */
            int       sbuf[2] = { ncg[cdd], nat[cdd] };
            if (debug)
            {
                fprintf(debug, "Sending ddim %d dir %d: ncg %d nat %d\n",
                        d, dir, sbuf[0], sbuf[1]);
            }
            int rbuf[2];
            ddSendrecv(dd, d, dir, sbuf, 2, rbuf, 2);

            flagBuffer.resize((ncg_recv + rbuf[0])*DD_CGIBS);

            /* Communicate the charge group indices, sizes and flags */
            ddSendrecv(dd, d, dir,
                       comm->cggl_flag[cdd].data(), sbuf[0]*DD_CGIBS,
                       flagBuffer.buffer.data() + ncg_recv*DD_CGIBS, rbuf[0]*DD_CGIBS);

            const int nvs = ncg[cdd] + nat[cdd]*nvec;
            const int i   = rbuf[0]  + rbuf[1] *nvec;
            rvecBuffer.resize(nvr + i);

            /* Communicate cgcm and state */
            ddSendrecv(dd, d, dir,
                       as_rvec_array(comm->cgcm_state[cdd].data()), nvs,
                       as_rvec_array(rvecBuffer.buffer.data()) + nvr, i);
            ncg_recv += rbuf[0];
            nvr      += i;
        }

        dd_check_alloc_ncg(fr, state, f, home_pos_cg + ncg_recv);
        if (fr->cutoff_scheme == ecutsGROUP)
        {
            /* Here we resize to more than necessary and shrink later */
            dd_resize_state(state, f, home_pos_at + ncg_recv*MAX_CGCGSIZE);
        }

        /* Process the received charge groups */
        int buf_pos = 0;
        for (int cg = 0; cg < ncg_recv; cg++)
        {
            int   flag = flagBuffer.buffer[cg*DD_CGIBS + 1];
            rvec &pos  = as_rvec_array(rvecBuffer.buffer.data())[buf_pos];

            if (dim >= npbcdim && dd->nc[dim] > 2)
            {
                rvec &pos = as_rvec_array(rvecBuffer.buffer.data())[buf_pos];

                /* No pbc in this dim and more than one domain boundary.
                 * We do a separate check if a charge group didn't move too far.
                 */
                if (((flag & DD_FLAG_FW(d)) &&
                     pos[dim] > cell_x1[dim]) ||
                    ((flag & DD_FLAG_BW(d)) &&
                     pos[dim] < cell_x0[dim]))
                {
                    cg_move_error(fplog, dd, step, cg, dim,
                                  (flag & DD_FLAG_FW(d)) ? 1 : 0,
                                  fr->cutoff_scheme == ecutsGROUP, 0,
                                  pos, pos, pos[dim]);
                }
            }

            int mc = -1;
            if (d < dd->ndim-1)
            {
                /* Check which direction this cg should go */
                for (int d2 = d + 1; (d2 < dd->ndim && mc == -1); d2++)
                {
                    if (isDlbOn(dd->comm))
                    {
                        /* The cell boundaries for dimension d2 are not equal
                         * for each cell row of the lower dimension(s),
                         * therefore we might need to redetermine where
                         * this cg should go.
                         */
                        const int dim2 = dd->dim[d2];
                        /* If this cg crosses the box boundary in dimension d2
                         * we can use the communicated flag, so we do not
                         * have to worry about pbc.
                         */
                        if (!((dd->ci[dim2] == dd->nc[dim2]-1 &&
                               (flag & DD_FLAG_FW(d2))) ||
                              (dd->ci[dim2] == 0 &&
                               (flag & DD_FLAG_BW(d2)))))
                        {
                            /* Clear the two flags for this dimension */
                            flag &= ~(DD_FLAG_FW(d2) | DD_FLAG_BW(d2));
                            /* Determine the location of this cg
                             * in lattice coordinates
                             */
                            real pos_d = rvecBuffer.buffer[buf_pos][dim2];
                            if (tric_dir[dim2])
                            {
                                for (int d3 = dim2+1; d3 < DIM; d3++)
                                {
                                    pos_d += pos[d3]*tcm[d3][dim2];
                                }
                            }

                            GMX_ASSERT(dim2 >= 0 && dim2 < DIM, "Keep the static analyzer happy");

                            /* Check of we are not at the box edge.
                             * pbc is only handled in the first step above,
                             * but this check could move over pbc while
                             * the first step did not due to different rounding.
                             */
                            if (pos_d >= cell_x1[dim2] &&
                                dd->ci[dim2] != dd->nc[dim2]-1)
                            {
                                flag |= DD_FLAG_FW(d2);
                            }
                            else if (pos_d < cell_x0[dim2] &&
                                     dd->ci[dim2] != 0)
                            {
                                flag |= DD_FLAG_BW(d2);
                            }
                            flagBuffer.buffer[cg*DD_CGIBS + 1] = flag;
                        }
                    }
                    /* Set to which neighboring cell this cg should go */
                    if (flag & DD_FLAG_FW(d2))
                    {
                        mc = d2*2;
                    }
                    else if (flag & DD_FLAG_BW(d2))
                    {
                        if (dd->nc[dd->dim[d2]] > 2)
                        {
                            mc = d2*2+1;
                        }
                        else
                        {
                            mc = d2*2;
                        }
                    }
                }
            }

            const int nrcg = flag & DD_FLAG_NRCG;
            if (mc == -1)
            {
                /* Set the global charge group index and size */
                const int globalAtomGroupIndex = flagBuffer.buffer[cg*DD_CGIBS];
                dd->globalAtomGroupIndices.push_back(globalAtomGroupIndex);
                dd->atomGrouping_.appendBlock(nrcg);
                /* Copy the state from the buffer */
                if (fr->cutoff_scheme == ecutsGROUP)
                {
                    cg_cm = fr->cg_cm;
                    copy_rvec(pos, cg_cm[home_pos_cg]);
                }
                buf_pos++;

                /* Set the cginfo */
                fr->cginfo[home_pos_cg] = ddcginfo(cginfo_mb,
                                                   globalAtomGroupIndex);
                if (comm->bLocalCG)
                {
                    comm->bLocalCG[globalAtomGroupIndex] = TRUE;
                }

                rvec *rvecPtr = as_rvec_array(rvecBuffer.buffer.data());
                for (int i = 0; i < nrcg; i++)
                {
                    copy_rvec(rvecPtr[buf_pos++],
                              state->x[home_pos_at+i]);
                }
                if (bV)
                {
                    for (int i = 0; i < nrcg; i++)
                    {
                        copy_rvec(rvecPtr[buf_pos++],
                                  state->v[home_pos_at+i]);
                    }
                }
                if (bCGP)
                {
                    for (int i = 0; i < nrcg; i++)
                    {
                        copy_rvec(rvecPtr[buf_pos++],
                                  state->cg_p[home_pos_at+i]);
                    }
                }
                home_pos_cg += 1;
                home_pos_at += nrcg;
            }
            else
            {
                /* Reallocate the buffers if necessary  */
                if ((ncg[mc] + 1)*DD_CGIBS > gmx::index(comm->cggl_flag[mc].size()))
                {
                    comm->cggl_flag[mc].resize((ncg[mc] + 1)*DD_CGIBS);
                }
                size_t nvr = ncg[mc] + nat[mc]*nvec;
                if (nvr + 1 + nrcg*nvec > comm->cgcm_state[mc].size())
                {
                    comm->cgcm_state[mc].resize(nvr + 1 + nrcg*nvec);
                }
                /* Copy from the receive to the send buffers */
                memcpy(comm->cggl_flag[mc].data() + ncg[mc]*DD_CGIBS,
                       flagBuffer.buffer.data() + cg*DD_CGIBS,
                       DD_CGIBS*sizeof(int));
                memcpy(comm->cgcm_state[mc][nvr],
                       rvecBuffer.buffer.data() + buf_pos,
                       (1 + nrcg*nvec)*sizeof(rvec));
                buf_pos += 1 + nrcg*nvec;
                ncg[mc] += 1;
                nat[mc] += nrcg;
            }
        }
    }

    /* Note that the indices are now only partially up to date
     * and ncg_home and nat_home are not the real count, since there are
     * "holes" in the arrays for the charge groups that moved to neighbors.
     */
    if (fr->cutoff_scheme == ecutsVERLET)
    {
        /* We need to clear the moved flags for the received atoms,
         * because the moved buffer will be passed to the nbnxn gridding call.
         */
        int *moved = getMovedBuffer(comm, dd->ncg_home, home_pos_cg);

        for (int i =  dd->ncg_home; i < home_pos_cg; i++)
        {
            moved[i] = 0;
        }
    }

    dd->ncg_home = home_pos_cg;
    comm->atomRanges.setEnd(DDAtomRanges::Type::Home, home_pos_at);

    if (fr->cutoff_scheme == ecutsGROUP)
    {
        /* We overallocated before, we need to set the right size here */
        dd_resize_state(state, f, comm->atomRanges.numHomeAtoms());
    }

    if (debug)
    {
        fprintf(debug,
                "Finished repartitioning: cgs moved out %d, new home %d\n",
                *ncg_moved, dd->ncg_home-*ncg_moved);

    }
}
