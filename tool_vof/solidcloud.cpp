#include <queue>
#include <cstdlib>
#include <iomanip>
#include <limits>

#include "Pstream.H"
#include "solidcloud.h"
#include "./libmotion/motionfactory.h"
#include "./libshape/shapefactory.h"
namespace sdfibm{

Foam::scalar calcLineFraction(const Foam::scalar& phia, const Foam::scalar& phib)
{
    // Foam::Info << phia << ' ' << phib << '\n';
    if(phia > 0 && phib > 0)
        return 0;
    if(phia < 0 && phib < 0)
        return 1;
    if(phia > 0) // phib < 0
        return -phib/(phia-phib);
    else
        return -phia/(phib-phia);
}

// ctor
SolidCloud::SolidCloud()
{
    m_field_name = "alpha.water";
    if(Foam::Pstream::master())
    {
        logfile.open("cloud.log");
        logfile << GenBanner("INIT SOLIDCLOUD");
    }
    m_solids.reserve(10);
    m_planes.reserve(10);

    m_ptr_Mesh = nullptr;
    m_ptr_As = nullptr;
    m_ptr_ct = nullptr;


    dictionary root(Foam::IFstream("solidDict")());


    if(Foam::Pstream::master())
        logfile << "reading solids from: " << root.name() << '\n';

    // meta information
    const dictionary& meta = root.subDict("meta");
    m_ON_TWOD  = Foam::readBool(meta.lookup("on_twod"));

    // output meta information
    if(Foam::Pstream::master())
    {
        logfile << GenBanner("SUMMARY");
        string dim = m_ON_TWOD ? "2D" : "3D";
        logfile << "Binary was compiled at " << __DATE__ << ' ' << __TIME__ << '\n';
        logfile << "Simulation starts   at " << GetTimeString() << '\n';
    }

   // read and create shape, motion, and material
   try {
        // read and create shapes
        if(Foam::Pstream::master())
        {
            logfile << GenBanner("CREATE: SHAPES");
            logfile << "--> Available shape types:\n";
            ShapeFactory::report(logfile);
            logfile << "--> Used shapes:\n";
        }
        const dictionary& shapes = root.subDict("shapes");
        m_radiusB = -1.0;
        for (int i=0; i < shapes.size(); ++i)
        {
            const dictionary& para = shapes.subDict(shapes.toc()[i]);
            std::string type = Foam::word(para.lookup("type"));
            std::string name = Foam::word(para.lookup("name"));

            m_libshape[name] = ShapeFactory::create(type, para);
            if(m_libshape[name] == nullptr)
                throw std::runtime_error(std::string("Unrecognized shape type " + type + '\n'));
            m_radiusB = std::max(m_radiusB, m_libshape[name]->getRadiusB());

            if(Foam::Pstream::master())
                logfile << "[+] " << type << " as " << name << " (" << m_libshape[name]->description() << ")\n";
        }
        // find the maximum bounding raidus
        if(Foam::Pstream::master())
        {
            logfile << "maximum bounding raidus is " << m_radiusB << '\n';
        }

        m_libmat["mat"] = new IMaterial(1.0);
    }
    catch (const std::exception& e)
    {
        if(Foam::Pstream::master())
        {
            std::cout << e.what();
            logfile << "Error when creating shape/motion/material!" << e.what() << '\n';
        }
        std::exit(1);
    }

    // create solids
    if(Foam::Pstream::master())
        logfile << GenBanner("CREATE: SOLIDS & PLANES");

    const dictionary &solids = root.subDict("solids");
    forAll(solids, i)
    {
        const dictionary &solid = solids.subDict(solids.toc()[i]);

        vector pos = solid.lookup("pos");
        if (m_ON_TWOD)
        {
            // sanity check for 2d simulation: solid must have z = 0
            if(pos.z() !=  0)
                Quit("Solid must has z=0 in 2D simulation, violated by solid # " + std::to_string(i));
        }
        // create solid
        Solid s(i, pos, quaternion::I);
        // read velocity, euler angle, angular velocity
        vector euler = solid.lookupOrDefault("euler", vector::zero);
        s.setOrientation(euler*M_PI/180.0);

        std::string shp_name = Foam::word(solid.lookup("shp_name"));
        s.setMaterialAndShape(m_libmat["mat"], m_libshape[shp_name]);
        this->addSolid(s);

        if(Foam::Pstream::master())
            logfile << "Solid " << i << " shape = " << shp_name << "\n";
    }

    const dictionary &planes = root.subDict("planes");
    forAll(planes, i)
    {
        const dictionary &plane = planes.subDict(planes.toc()[i]);

        vector pos = plane.lookup("pos");
        if (m_ON_TWOD)
        {
            // sanity check for 2d simulation: plane must have z = 0
            if(pos.z() !=  0)
                Quit("Plane must has z=0 in 2D simulation, violated by plane # " + std::to_string(i));
        }
        // create plane
        Solid s(i, pos, quaternion::I);
        // read velocity, euler angle, angular velocity
        vector euler = plane.lookupOrDefault("euler", vector::zero);
        s.setOrientation(euler*M_PI/180.0);

        std::string shp_name = Foam::word(plane.lookup("shp_name"));
        s.setMaterialAndShape(m_libmat["mat"], m_libshape[shp_name]);
        this->addPlane(s);

        if(Foam::Pstream::master())
            logfile << "Plane " << i << " shape = " << shp_name << "\n";
    }

    if(Foam::Pstream::master())
    {
        logfile << "Totally [" << m_solids.size() << "] solids and [" << m_planes.size() << "] planes.\n";
        logfile << GenBanner("END OF INIT");
    }
}

void SolidCloud::solidFluidInteract(Solid& solid, const real& dt)
{
    // interaction of a single solid with fluid
    const Foam::fvMesh&       mesh = *m_ptr_Mesh;
    const Foam::pointField &    pp = mesh.points();
    const Foam::vectorField&    cc = mesh.cellCentres();

    const Foam::vectorField& faceCentres = mesh.faceCentres();
    const Foam::vectorField& faceAreas   = mesh.faceAreas();
    // connectivity
    const Foam::labelListList& c2c = mesh.cellCells();
    const Foam::labelListList& c2p = mesh.cellPoints();

    Foam::volScalarField& ct = *m_ptr_ct; // cell type field
    Foam::volScalarField& As = *m_ptr_As;

    // handle host cell
    int hostid = m_ms->findCell(solid.getCenter());
    if(hostid < 0)
    {
        // fallback to more expensive option, scan cells of the current partition to find the hostid
        forAll(cc, icell)
        {
            if(solid.isInside(cc[icell]))
            {
                int insideCount = 0;
                // loop cell vertices
                forAll(c2p[icell], ivert)
                    insideCount += solid.isInside(pp[c2p[icell][ivert]]);
                if(insideCount == c2p[icell].size()) // equals #vertex
                {
                    hostid = icell;
                    break;
                }
            }
        }
    }

    if(hostid>=0) // solid overlaps partition
    {
        label innerType = 4 + solid.getID();
        ct[hostid] = innerType;

        // use breadth first search (BFS) to visit cells intersected by the solid
        std::queue<int> q;
        q.push(hostid);
        std::vector<int> intersectCell;
        std::vector<int> visitedOutCell;

        while(!q.empty())
        {
            // icur is the index of the current cell
            int icur = q.front();
            q.pop();

            // process current cell
            if(ct[icur] == innerType)
            {
                As[icur] = 1.0;
            }

            // loop neighbors of the current cell
            forAll(c2c[icur], itmp)
            {
                Foam::label inb = c2c[icur][itmp];
                if(ct[inb] == 0) // unvisited nb cell
                {
                    // detailed check of this nb cell
                    int insideCount = 0;
                    // loop cell vertices
                    forAll(c2p[inb], ivert)
                    {
                        insideCount += solid.isInside(pp[c2p[inb][ivert]]);
                    }

                    if(insideCount == 0)
                    {
                        // cell totally outside of solid, no need to proceed, but we still need to mark it as "visited"
                        ct[inb] = 1;
                        visitedOutCell.push_back(inb);
                        continue;
                    }

                    // at this point the cell is both unvisited & intersected for sure, so we add it to the queue
                    q.push(inb);

                    if(insideCount == c2p[inb].size())
                    {
                        ct[inb] = innerType;
                    }
                    else
                    {
                        intersectCell.push_back(inb);
                        // check cell center
                        if(solid.isInside(cc[inb]))
                            ct[inb] = 3; // cell intersected and has centroid inside solid
                        else
                            ct[inb] = 2; // cell intersected but has centroid outside of solid
                    }
                }
            }
        }
        for(label icur : intersectCell)
            ct[icur] = 0;
        for(label icur : visitedOutCell)
            ct[icur] = 0;

        for(label icur : intersectCell)
        {
            // build a hash map first to store the sdf of all vertices
            std::map<int, Foam::scalar> sdf_values; // vertex-id : value pairs
            const Foam::cell& cell = mesh.cells()[icur];
            const Foam::labelList& pointids = c2p[icur];
            forAll(pointids, ipoint)
            {
                sdf_values[pointids[ipoint]] = solid.getShape()->signedDistance(pp[pointids[ipoint]], solid.getCenter(), solid.getOrientation());
            }

            Foam::vector pcell;
            const Foam::vector& A = pp[pointids[0]];
            Foam::scalar phiA = sdf_values[pointids[0]];
            int i;
            Foam::vector B = Foam::vector::zero;
            Foam::scalar phiB = 0.0;
            for(i = 1; i < pointids.size(); ++i)
            {
                B = pp[pointids[i]];
                phiB = sdf_values[pointids[i]];
                if (phiA * phiB < 0)
                    break;
            }
            pcell = A - std::fabs(phiA)/(std::fabs(phiA)+std::fabs(phiB))*(A-B);
            if(m_ON_TWOD)
                pcell[2] = 0.0;

            Foam::scalar volume = 0.0;
            forAll(cell, iface)
            {
                Foam::label faceid = cell[iface];
                Foam::face myface = mesh.faces()[faceid];

                Foam::label nvertex = myface.size();
                std::vector<Foam::scalar> phis(nvertex);
                forAll(myface, ivertex)
                {
                    phis[ivertex] = sdf_values[myface[ivertex]];
                }

                Foam::scalar epsilon_f = 0.0;

                int sign_sum = 0;
                forAll(myface, ivertex)
                {
                    if(phis[ivertex] > 0)
                        sign_sum += 1;
                    else
                        sign_sum -= 1;
                }

                if(sign_sum == nvertex)
                    epsilon_f = 0.0;
                else if(sign_sum ==-nvertex)
                    epsilon_f = 1.0;
                else
                {
                    const Foam::vector& A = pp[myface[0]];
                    Foam::scalar phiA = sdf_values[myface[0]];

                    Foam::vector B = Foam::vector::zero;
                    Foam::scalar phiB = 0.0;
                    int i;
                    for(i = 1; i < nvertex; ++i)
                    {
                        B = pp[myface[i]];
                        phiB = sdf_values[myface[i]];
                        if (phiA * phiB < 0)
                            break;
                    }

                    Foam::vector pface = A - phiA/(phiA-phiB)*(A-B);

                    Foam::scalar area = 0.0;
                    for(int iseg = 0; iseg < nvertex; ++iseg)
                    {
                        const Foam::scalar& phiO = phis[iseg];
                        const Foam::scalar& phiA = phis[(iseg+1)%nvertex];
                        const Foam::vector& O = pp[myface[iseg]];
                        const Foam::vector& A = pp[myface[(iseg+1)%nvertex]];
                        area += std::fabs(0.5*Foam::mag((A-O) ^ (pface-O))) * calcLineFraction(phiO, phiA);
                    }
                    epsilon_f = area/Foam::mag(faceAreas[faceid]);
                }
                volume += (1.0/3.0)*epsilon_f*std::fabs((pcell - faceCentres[faceid]) & faceAreas[faceid]);
            }
            Foam::scalar alpha = volume/mesh.V()[icur];

            As[icur] = alpha;
        }
    }
}

void SolidCloud::writeVOF(const Foam::fvMesh& mesh)
{
    m_ptr_Mesh= &(const_cast<Foam::fvMesh&>(mesh));
    m_ptr_As  = &(const_cast<Foam::volScalarField&>(mesh.lookupObject<Foam::volScalarField>(m_field_name)));
    m_ptr_ct  = &(const_cast<Foam::volScalarField&>(mesh.lookupObject<Foam::volScalarField>("Ct")));

    m_ms = new Foam::meshSearch(mesh);

    (*m_ptr_As) = 0.0;
    (*m_ptr_ct) = 0;

    for(Solid& solid : m_solids)
        solidFluidInteract(solid, 0.0);

    for(Solid& solid : m_planes)
        solidFluidInteract(solid, 0.0);

    m_ptr_As->correctBoundaryConditions();


    forAll(m_ptr_Mesh->cells(), icell)
    {
        real alpha = m_ptr_As->operator[](icell);
        if(alpha < 0 || alpha > 1 + 1e-6)
           Foam::Info << "Wrong volume fraction " << alpha << " at cell " << icell << '\n';
    }

    m_ptr_As->write(); // write to the zero directory
    const Foam::scalarField& cell_vol = m_ptr_Mesh->V();
    Foam::Info << "* Write smooth phase field [" << m_field_name << "] to ./0, total volume = " << Foam::gSum((*m_ptr_As)*cell_vol) << '\n';
    Foam::Info << "* Finished at " << sdfibm::GetTimeString() << '\n';

    delete m_ms; m_ms = nullptr;
    logfile.close();
}

}
