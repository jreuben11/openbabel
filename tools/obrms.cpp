/**********************************************************************
 obrms = Returns the rms between two chemically identical structures
 Derived from obfit.
 *
 This file is part of the Open Babel project.
 For more information, see <http://openbabel.org/>
 
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation version 2 of the License.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 ***********************************************************************/

/*
 Require a fixed molecule and a set of molecules to compare against.
 example of command line:
 obrms ref.sdf test.sdf
 */

// used to set import/export for Cygwin DLLs
#ifdef WIN32
#define USING_OBDLL
#endif

#include <openbabel/babelconfig.h>
#include <openbabel/mol.h>
#include <openbabel/parsmart.h>
#include <openbabel/obconversion.h>
#include <openbabel/query.h>
#include <openbabel/isomorphism.h>

#ifndef _MSC_VER
#include <unistd.h>
#endif

#include <sstream>
#include <boost/unordered_map.hpp>
#include <boost/program_options.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_stream.hpp>

using namespace std;
using namespace boost;
using namespace boost::program_options;
using namespace OpenBabel;

class AtomDistanceSorter
{
	vector3 ref;
	public:
	AtomDistanceSorter(OBAtom *r) :
			ref(r->GetVector())
	{

	}
	bool operator()(OBAtom *l, OBAtom *r) const
			{
		double ld = ref.distSq(l->GetVector());
		double rd = ref.distSq(r->GetVector());

		return ld < rd;
	}
};
/* class to perform graph matching between two molecules.
 * Is initialized with the reference molecule.
 * Will figure out the atom correspondences and compute the rmsd between the
 * ref mol and a provided test mol.
 */
class Matcher
{
	const OBMol& ref;
	OBQuery *query;
	OBIsomorphismMapper *mapper;

	class MapRMSDFunctor: public OBIsomorphismMapper::Functor
	{
	private:
		const OBMol& ref;
		const OBMol& test;
		double bestRMSD;
		bool minimize;
		public:
		MapRMSDFunctor(const OBMol& r, const OBMol& t, bool min = false) :
				ref(r), test(t), bestRMSD(HUGE_VAL), minimize(min)
		{
		}

		bool operator()(OBIsomorphismMapper::Mapping &map)
		{
			unsigned N = map.size();
			double refcoord[N * 3];
			double testcoord[N * 3];

			for (unsigned i = 0; i < N; i++)
			{
				//obmol indices are 1-indexed while the mapper is zero indexed
				const OBAtom *ratom = ref.GetAtom(map[i].first + 1);
				const OBAtom *tatom = test.GetAtom(map[i].second + 1);
				assert(ratom && tatom);

				for (unsigned c = 0; c < 3; c++)
				{
					refcoord[3 * i + c] = ratom->GetVector()[c];
					testcoord[3 * i + c] = tatom->GetVector()[c];
				}
			}

			if (minimize)
			{
				double rmatrix[3][3] =
						{ 0 };

				double rave[3] =
						{ 0, 0, 0 };
				double tave[3] =
						{ 0, 0, 0 };
				//center
				for (unsigned i = 0; i < N; i++)
				{
					for (unsigned c = 0; c < 3; c++)
					{
						rave[c] += refcoord[3 * i + c];
						tave[c] += testcoord[3 * i + c];
					}
				}

				for (unsigned c = 0; c < 3; c++)
				{
					rave[c] /= N;
					tave[c] /= N;
				}

				for (unsigned i = 0; i < N; i++)
				{
					for (unsigned c = 0; c < 3; c++)
					{
						refcoord[3 * i + c] -= rave[c];
						testcoord[3 * i + c] -= tave[c];
					}
				}

				qtrfit(refcoord, testcoord, N, rmatrix);
				rotate_coords(testcoord, rmatrix, N);
			}

			double rmsd = calc_rms(refcoord, testcoord, N);

			if (rmsd < bestRMSD)
				bestRMSD = rmsd;
			// check all possible mappings
			return false;
		}

		double getRMSD() const
		{
			return bestRMSD;
		}
	};

public:
	Matcher(OBMol& mol) :
			ref(mol), query(NULL), mapper(NULL)
	{
		query = CompileMoleculeQuery(&mol);
		mapper = OBIsomorphismMapper::GetInstance(query);
	}

	~Matcher()
	{
		if (query)
			delete query;
		if (mapper)
			delete mapper;
	}

	//computes a correspondence between the ref mol and test (exhaustively)
	//and returns the rmsd; returns infinity if unmatchable
	double computeRMSD(OBMol& test, bool minimize = false)
	{
		MapRMSDFunctor funct(ref, test, minimize);

		mapper->MapGeneric(funct, &test);
		return funct.getRMSD();
	}
};

//preprocess molecule into a standardized state for heavy atom rmsd computation
static void processMol(OBMol& mol)
{
	//isomorphismmapper wants isomorphic atoms to have the same aromatic and ring state,
	//but these proporties aren't reliable enough to be trusted in evaluating molecules
	//should be considered the same based solely on connectivity

	mol.DeleteHydrogens(); //heavy atom rmsd
	for (OBAtomIterator aitr = mol.BeginAtoms(); aitr != mol.EndAtoms(); aitr++)
	{
		OBAtom *a = *aitr;
		a->UnsetAromatic();
		a->SetInRing();
	}
	for (OBBondIterator bitr = mol.BeginBonds(); bitr != mol.EndBonds(); bitr++)
	{
		OBBond *b = *bitr;
		b->UnsetAromatic();
		b->SetBondOrder(1);
		b->SetInRing();
	}
	//avoid recomputations
	mol.SetHybridizationPerceived();
	mol.SetRingAtomsAndBondsPerceived();
	mol.SetAromaticPerceived();
}
///////////////////////////////////////////////////////////////////////////////
//! \brief compute rms between chemically identical molecules
int main(int argc, char **argv)
{
	bool firstOnly = false;
	bool minimize = false;
	bool help = false;
	string fileRef;
	string fileTest;

	program_options::options_description desc("Allowed options");
	desc.add_options()
	("reference", value<string>(&fileRef)->required(),
			"reference structure(s) file")
	("test", value<string>(&fileTest)->required(), "test structure(s) file")
	("firstonly,f", bool_switch(&firstOnly),
			"use only the first structure in the reference file")
	("minimize,m", bool_switch(&minimize), "compute minimum RMSD")
	("help", bool_switch(&help), "produce help message");

	positional_options_description pd;
	pd.add("reference", 1).add("test", 1);

	variables_map vm;
	try
	{
		store(
				command_line_parser(argc, argv).options(desc).positional(pd).run(),
				vm);
		notify(vm);
	} catch (boost::program_options::error& e)
	{
		std::cerr << "Command line parse error: " << e.what() << '\n' << desc
				<< '\n';
		exit(-1);
	}

	if (help)
	{
		cout
		<< "Computes the heavy-atom RMSD of identical compound structures.\n";
		cout << desc;
		exit(0);
	}

	//open mols
	OBConversion refconv;
	OBFormat *refFormat = refconv.FormatFromExt(fileRef);
	if (!refFormat || !refconv.SetInFormat(refFormat)
			|| !refconv.SetOutFormat("SMI"))
	{
		cerr << "Cannot read reference molecule format!" << endl;
		exit(-1);
	}

	OBConversion testconv;
	OBFormat *testFormat = testconv.FormatFromExt(fileTest);
	if (!testFormat || !testconv.SetInAndOutFormats(testFormat, testFormat))
	{
		cerr << "Cannot read reference molecule format!" << endl;
		exit(-1);
	}

	//read reference
	OBMol molref;
	std::ifstream uncompressed_inmol(fileRef.c_str());
	iostreams::filtering_stream<iostreams::input> ifsref;
	string::size_type pos = fileRef.rfind(".gz");
	if (pos != string::npos)
	{
		ifsref.push(iostreams::gzip_decompressor());
	}
	ifsref.push(uncompressed_inmol);

	if (!ifsref || !uncompressed_inmol)
	{
		cerr << "Cannot read fixed molecule file: " << fileRef << endl;
		exit(-1);
	}

	//check comparison file
	std::ifstream uncompressed_test(fileTest.c_str());
	iostreams::filtering_stream<iostreams::input> ifstest;
	pos = fileTest.rfind(".gz");
	if (pos != string::npos)
	{
		ifstest.push(iostreams::gzip_decompressor());
	}
	ifstest.push(uncompressed_test);

	if (!ifstest || !uncompressed_test)
	{
		cerr << "Cannot read file: " << fileTest << endl;
		exit(-1);
	}

	while (refconv.Read(&molref, &ifsref))
	{
		processMol(molref);
		Matcher matcher(molref); // create the matcher
		OBMol moltest;
		while (testconv.Read(&moltest, &ifstest))
		{
			if (moltest.Empty())
				break;

			processMol(moltest);

			double rmsd = matcher.computeRMSD(moltest, minimize);

			cout << "RMSD " << moltest.GetTitle() << " " << rmsd << "\n";
			if (!firstOnly)
			{
				break;
			}
		}
	}
	return (0);
}
