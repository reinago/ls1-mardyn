/*
 * DistControl.cpp
 *
 *  Created on: 16.03.2015
 *      Author: mheinen
 */

#include "DistControl.h"
#include "molecules/Molecule.h"
#include "Domain.h"
#include "particleContainer/ParticleContainer.h"
#include "parallel/DomainDecompBase.h"
#include "utils/Region.h"
#include "utils/DynAlloc.h"
#include "utils/Math.h"
#include "utils/xmlfileUnits.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <numeric>
#include <limits>
#include <cstdlib>
#include <cstdint>
#include <cmath>

using namespace std;


DistControl::DistControl(DomainDecompBase* domainDecomp, Domain* domain)
		: ControlInstance(domain, domainDecomp),
		  _nFlag(DCF_DEFAULT)
{
	// number of components
	_nNumComponents = this->GetDomain()->getNumberOfComponents() + 1;  // +1: because 0 stands for sum of all components;

	// init data structure pointers
	this->InitDataStructurePointers();
}

DistControl::DistControl(DomainDecompBase* domainDecomp, Domain* domain, unsigned int nUpdateFreq, unsigned int nWriteFreqProfiles)
		: ControlInstance(domain, domainDecomp), _dInterfaceMidLeft(0.), _dInterfaceMidRight(0.),
		_nNumComponents(0), _nTargetCompID(0), _nNumValuesScalar(0), _nOffsets(NULL)
{
	// number of components
	_nNumComponents = this->GetDomain()->getNumberOfComponents() + 1;  // +1: because 0 stands for sum of all components;

	// write data
	_nWriteFreq = _nUpdateFreq;

	_strFilename = "DistControl.dat";
	_strFilenameProfilesPrefix = "DistControlProfiles";

	// init method variables
	_strFilenameInit = "unknown";
	_nRestartTimestep = 0;

	// init data structure pointers
	this->InitDataStructurePointers();
}

DistControl::~DistControl()
{
	// free memory
	delete [] _nOffsets;
	delete [] _nNumMoleculesLocal;
	delete [] _nNumMoleculesGlobal;
	delete [] _dMidpointPositions;
	delete [] _dForceSumLocal;
	delete [] _dForceSumGlobal;
	delete [] _dDensityProfile;
	delete [] _dDensityProfileSmoothed;
	delete [] _dDensityProfileSmoothedDerivation;
	delete [] _dForceProfile;
	delete [] _dForceProfileSmoothed;
	// align COM
	delete [] _nNumMoleculesCOMLocal;
	delete [] _nNumMoleculesCOMGlobal;
	delete [] _dPosSumLocal;
	delete [] _dPosSumGlobal;
}

void DistControl::readXML(XMLfileUnits& xmlconfig)
{
	// flag
	std::string strFlag = "unknown";
	xmlconfig.getNodeValue("@flag", strFlag);
	if("DCF_ALIGN_COM_ONLY" == strFlag)
		_nFlag = DCF_ALIGN_COM_ONLY;
	else
		_nFlag = DCF_DEFAULT;

	// control
	_nUpdateFreq = 5000;
	_nWriteFreqProfiles = 50000;
	xmlconfig.getNodeValue("control/update", _nUpdateFreq);
	xmlconfig.getNodeValue("control/writeprofiles", _nWriteFreqProfiles);
	_nWriteFreq = _nUpdateFreq;

	// filenames
	_strFilename = "DistControl.dat";
	_strFilenameProfilesPrefix = "DistControlProfiles";
	xmlconfig.getNodeValue("filenames/control",  _strFilename);
	xmlconfig.getNodeValue("filenames/profiles", _strFilenameProfilesPrefix);
	global_log->error() << "DistControl: Writing control data to file: " << _strFilename << endl;
	global_log->error() << "DistControl: Writing profile data to files with prefix: " << _strFilenameProfilesPrefix << endl;

	// subdivision of system
	uint32_t nSubdivisionType = SDOPT_UNKNOWN;
	std::string strSubdivisionType;
	if( !xmlconfig.getNodeValue("subdivision@type", strSubdivisionType) )
	{
		global_log->error() << "DistControl: Missing attribute \"subdivision@type\"! Programm exit..." << endl;
		exit(-1);
	}
	if("number" == strSubdivisionType)
	{
		unsigned int nNumSlabs = 0;
		if( !xmlconfig.getNodeValue("subdivision/number", nNumSlabs) )
		{
			global_log->error() << "DistControl: Missing element \"subdivision/number\"! Programm exit..." << endl;
			exit(-1);
		}
		else
			this->SetSubdivision(nNumSlabs);
	}
	else if("width" == strSubdivisionType)
	{
		double dSlabWidth = 0.;
		if( !xmlconfig.getNodeValue("subdivision/width", dSlabWidth) )
		{
			global_log->error() << "DistControl: Missing element \"subdivision/width\"! Programm exit..." << endl;
			exit(-1);
		}
		else
			this->SetSubdivision(dSlabWidth);
	}
	else
	{
		global_log->error() << "DistControl: Wrong attribute \"subdivision@type\". Expected: type=\"number|width\"! Programm exit..." << endl;
		exit(-1);
	}

	// init method
	_nMethodInit = DCIM_UNKNOWN;
	_dInterfaceMidLeft  = 10.0;
	_dInterfaceMidRight = 20.0;
	_strFilenameInit = "unknown";
	_nRestartTimestep = 1;
	std::string strInitMethodType;

	xmlconfig.getNodeValue("init@type", strInitMethodType);
	if("startconfig" == strInitMethodType)
	{
		_nMethodInit = DCIM_START_CONFIGURATION;
		global_log->info() << "DistControl: Init method 'startconfig', dertermining interface midpoints from start configuration." << endl;
	}
	else if("values" == strInitMethodType)
	{
		_nMethodInit = DCIM_MIDPOINT_VALUES;
		bool bInputIsValid = true;
		bInputIsValid = bInputIsValid && xmlconfig.getNodeValue("init/values/left",  _dInterfaceMidLeft);
		bInputIsValid = bInputIsValid && xmlconfig.getNodeValue("init/values/right", _dInterfaceMidRight);
		if(true == bInputIsValid)
		{
			global_log->info() << "DistControl: Init method 'values' => interface midpoint left: " << _dInterfaceMidLeft << ", "
					"right: " << _dInterfaceMidRight << "." << endl;
		}
		else
		{
			global_log->error() << "DistControl: Missing elements \"init/values/left\" or \"init/values/right\" or both! Programm exit..." << endl;
			exit(-1);
		}
	}
	else if("file" == strInitMethodType)
	{
		_nMethodInit = DCIM_READ_FROM_FILE;
		bool bInputIsValid = true;
		bInputIsValid = bInputIsValid && xmlconfig.getNodeValue("init/file",    _strFilenameInit);
		bInputIsValid = bInputIsValid && xmlconfig.getNodeValue("init/simstep", _nRestartTimestep);
		if(true == bInputIsValid)
		{
			global_log->info() << "DistControl: Init method 'file', reading from file: " << _strFilenameInit << ", "
					"goto line with simstep == " << _nRestartTimestep << "." << endl;
		}
		else
		{
			global_log->error() << "DistControl: Missing elements \"init/file\" or \"init/simstep\" or both! Programm exit..." << endl;
			exit(-1);
		}
	}
	else
	{
		global_log->error() << "DistControl: Wrong attribute \"init@type\", type = " << strInitMethodType << ", "
				"expected: type=\"startconfig|values|file\"! Programm exit..." << endl;
		exit(-1);
	}

	// update method
	_nMethod = DCUM_UNKNOWN;
	_nTargetCompID = 0;
	_dVaporDensity = 0.01;
	_nNeighbourValsSmooth   = 2;
	_nNeighbourValsDerivate = 2;
	std::string strUpdateMethodType;

	xmlconfig.getNodeValue("method@type", strUpdateMethodType);
	if("density" == strUpdateMethodType)
	{
		_nMethod = DCUM_DENSITY_PROFILE;
		bool bInputIsValid = true;
		uint32_t nTargetCompID = 0;
		bInputIsValid = bInputIsValid && xmlconfig.getNodeValue("method/componentID", nTargetCompID);
		_nTargetCompID = (unsigned short)(nTargetCompID);
		bInputIsValid = bInputIsValid && xmlconfig.getNodeValue("method/density",     _dVaporDensity);
		if(true == bInputIsValid)
		{
			global_log->info() << "DistControl: Update method 'density', using constant value for vapor density rho_vap == " << _dVaporDensity << ", "
					"target componentID: " << _nTargetCompID << "." << endl;
		}
		else
		{
			global_log->error() << "DistControl: Missing elements \"method/componentID\" or \"method/density\" or both! Programm exit..." << endl;
			exit(-1);
		}
	}
	else if("denderiv" == strUpdateMethodType)
	{
		_nMethod = DCUM_DENSITY_PROFILE_DERIVATION;
		bool bInputIsValid = true;
		uint32_t nTargetCompID = 0;
		uint32_t nNeighbourValsSmooth = 0;
		uint32_t nNeighbourValsDerivate = 0;
		bInputIsValid = bInputIsValid && xmlconfig.getNodeValue("method/componentID", nTargetCompID);
		bInputIsValid = bInputIsValid && xmlconfig.getNodeValue("method/neighbourvals[@algorithm='smooth']",   nNeighbourValsSmooth);
		bInputIsValid = bInputIsValid && xmlconfig.getNodeValue("method/neighbourvals[@algorithm='derivate']", nNeighbourValsDerivate);
		_nTargetCompID = (unsigned short)(nTargetCompID);
		_nNeighbourValsSmooth = (unsigned short)(nNeighbourValsSmooth);
		_nNeighbourValsDerivate = (unsigned short)(nNeighbourValsDerivate);
		if(true == bInputIsValid)
		{
			global_log->info() << "DistControl: Update method 'denderiv', using " << _nNeighbourValsSmooth << " neigbour values for smoothing "
					" and " << _nNeighbourValsDerivate << " neigbour values for derivation of the density profile, target componentID: " << _nTargetCompID << "." << endl;
		}
		else
		{
			global_log->error() << "DistControl: Missing elements \"method/componentID\" or \"method/density\" or both! Programm exit..." << endl;
			exit(-1);
		}
	}
	else
	{
		global_log->error() << "DistControl: Wrong attribute \"method@type\", type = " << strUpdateMethodType << ", "
				"expected: type=\"density|denderiv\"! Programm exit..." << endl;
		exit(-1);
	}

	// align COM
	double dBoxWidth[3];
	for(uint8_t d=0; d<3; ++d)
		dBoxWidth[d] = this->GetDomain()->getGlobalLength(d);
	std::string strRelation;
	unsigned int nCompIDTargetCOM;
	xmlconfig.getNodeValue("alignCOM/componentID", nCompIDTargetCOM);
	_nCompIDTargetCOM = (uint8_t)(nCompIDTargetCOM);
	xmlconfig.getNodeValue("alignCOM/coords@relation", strRelation);
	xmlconfig.getNodeValue("alignCOM/coords/x", _dPosTargetCOM[0]);
	xmlconfig.getNodeValue("alignCOM/coords/y", _dPosTargetCOM[1]);
	xmlconfig.getNodeValue("alignCOM/coords/z", _dPosTargetCOM[2]);
	if("box" == strRelation)
		for(uint8_t d=0; d<3; ++d)
			_dPosTargetCOM[d] *= dBoxWidth[d];
}

void DistControl::PrepareSubdivision()
{
	Domain* domain = this->GetDomain();
	double dWidth = domain->getGlobalLength(1);

	switch(_nSubdivisionOpt)
	{
	case SDOPT_BY_NUM_SLABS:
		_dShellWidth = dWidth / ( (double)(_nNumShells) );
		break;
	case SDOPT_BY_SLAB_WIDTH:
		_nNumShells = round(dWidth / _dShellWidth);
		_dShellWidth = dWidth / ( (double)(_nNumShells) );
		break;
	case SDOPT_UNKNOWN:
	default:
		global_log->error() << "ERROR in DistControl::PrepareSubdivision(): Neither _dShellWidth nor _nNumShells was set correctly! Programm exit..." << endl;
		exit(-1);
	}

	_dInvertShellWidth = 1. / _dShellWidth;
    _dShellVolume = _dShellWidth * domain->getGlobalLength(0) * domain->getGlobalLength(2);

//	cout << "DistControl::_nNumShells = " << _nNumShells << endl;
}

void DistControl::InitDataStructurePointers()
{
    // number of molecules
	_nNumMoleculesLocal = NULL;
	_nNumMoleculesGlobal = NULL;

	// density profile
	_dDensityProfile = NULL;
	_dDensityProfileSmoothed = NULL;
	_dDensityProfileSmoothedDerivation = NULL;

    // force profile
	_dForceSumLocal = NULL;
	_dForceSumGlobal = NULL;
	_dForceProfile = NULL;
	_dForceProfileSmoothed = NULL;

	// profile midpoint positions
	_dMidpointPositions = NULL;

	// align COM
	_nNumMoleculesCOMLocal = NULL;
	_nNumMoleculesCOMGlobal = NULL;
	_dPosSumLocal = NULL;
	_dPosSumGlobal = NULL;
}

void DistControl::AllocateDataStructures()
{
    // number of molecules
	AllocateUnsLongArray(_nNumMoleculesLocal, _nNumValuesScalar);
	AllocateUnsLongArray(_nNumMoleculesGlobal, _nNumValuesScalar);

	// density profile
	AllocateDoubleArray(_dDensityProfile, _nNumValuesScalar);
	AllocateDoubleArray(_dDensityProfileSmoothed, _nNumValuesScalar);
	AllocateDoubleArray(_dDensityProfileSmoothedDerivation, _nNumValuesScalar);

    // force profile
	AllocateDoubleArray(_dForceSumLocal, _nNumValuesScalar);
	AllocateDoubleArray(_dForceSumGlobal, _nNumValuesScalar);
	AllocateDoubleArray(_dForceProfile, _nNumValuesScalar);
	AllocateDoubleArray(_dForceProfileSmoothed, _nNumValuesScalar);

	// profile midpoint positions
	AllocateDoubleArray(_dMidpointPositions, _nNumShells);

	// align COM
	AllocateUint64Array(_nNumMoleculesCOMLocal, _nNumComponents);
	AllocateUint64Array(_nNumMoleculesCOMGlobal, _nNumComponents);
	AllocateDoubleArray(_dPosSumLocal, _nNumComponents*3);
	AllocateDoubleArray(_dPosSumGlobal, _nNumComponents*3);
}

void DistControl::InitDataStructures()
{
	// profile midpoint positions
	for(unsigned int s = 0; s < _nNumShells; ++s)
		_dMidpointPositions[s] = (0.5 + s)*_dShellWidth;

	for(unsigned int s = 0; s < _nNumValuesScalar; ++s)
	{
		// number of molecules
		_nNumMoleculesLocal[s] = 0;
		_nNumMoleculesGlobal[s] = 0;

		// density profile
		_dDensityProfile[s] = 0.;
		_dDensityProfileSmoothed[s] = 0.;
		_dDensityProfileSmoothedDerivation[s] = 0.;

		// force profile
		_dForceSumLocal[s] = 0.;
		_dForceSumGlobal[s] = 0.;
		_dForceProfile[s] = 0.;
		_dForceProfileSmoothed[s] = 0.;
	}

	// align COM
	for(uint8_t c=0; c<_nNumComponents; ++c)
	{
		_nNumMoleculesCOMLocal[c] = 0;
		_nNumMoleculesCOMGlobal[c] = 0;
		for(uint8_t d=0; d<3; ++d)
		{
			_dPosSumLocal[c+d] = 0.;
			_dPosSumGlobal[c+d] = 0.;
		}
	}
}

void DistControl::PrepareDataStructures()
{
	_nNumValuesScalar = _nNumShells * _nNumComponents;
//	cout << "DC: _nNumValuesScalar = " << _nNumValuesScalar << endl;
//	cout << "DC: _nNumComponents = " << (uint32_t)_nNumComponents << endl;

	// init offset array
	_nOffsets = new unsigned long[_nNumComponents];
	for(unsigned short cid=0; cid<_nNumComponents; ++cid)
		_nOffsets[cid] = cid*_nNumShells;

	this->AllocateDataStructures();
	this->InitDataStructures();
}

// update method
void DistControl::SetUpdateMethod(const int& nMethod, const unsigned short& nVal1, const unsigned short& nVal2, const unsigned short& nVal3, const double& dVal)
{
	_nMethod = nMethod;

	switch(_nMethod)
	{
	case DCUM_DENSITY_PROFILE:
		_dVaporDensity = dVal;
		_nTargetCompID = nVal3;
		break;
	case DCUM_DENSITY_PROFILE_DERIVATION:
		_nNeighbourValsSmooth = nVal1;
		_nNeighbourValsDerivate = nVal2;
		_nTargetCompID = nVal3;
		break;
	}
}

void DistControl::SetInitMethod(const int& nMethod, const double& dVal1, const double& dVal2, std::string strVal, const unsigned long& nVal)
{
	_nMethodInit = nMethod;

	switch(_nMethodInit)
	{
	case DCIM_MIDPOINT_VALUES:
		_dInterfaceMidLeft  = dVal1;
		_dInterfaceMidRight = dVal2;
		break;
	case DCIM_READ_FROM_FILE:
		_strFilenameInit = strVal;
		_nRestartTimestep = nVal;
		break;
	case DCIM_START_CONFIGURATION:
	case DCIM_UNKNOWN:
	default:
		break;
	}
}

void DistControl::SampleProfiles(Molecule* mol)
{
    unsigned int nPosIndex;
    unsigned int nIndexMax = _nNumShells - 1;

    // calc position index
    double dPos = mol->r(1);

    nPosIndex = (unsigned int) floor(dPos * _dInvertShellWidth);

    // ignore outer (halo) molecules
    if(nPosIndex > nIndexMax)  // negative values will be ignored to: cast to unsigned int --> high value
        return;

    _nNumMoleculesLocal[nPosIndex]++;
    _dForceSumLocal[nPosIndex] += mol->F(1);

	unsigned short cid = mol->componentid() + 1;  // cid == 0: sum of all components
	unsigned long nOffset = _nOffsets[cid] + nPosIndex;
	_nNumMoleculesLocal[nOffset]++;
	_dForceSumLocal[nOffset] += mol->F(1);
}

void DistControl::CalcProfiles()
{
	#ifdef ENABLE_MPI

    MPI_Allreduce( _nNumMoleculesLocal, _nNumMoleculesGlobal, _nNumValuesScalar, MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
    MPI_Allreduce( _dForceSumLocal, _dForceSumGlobal, _nNumValuesScalar, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

#else
    for(unsigned int s = 0; s < _nNumValuesScalar; ++s)
    {
        _nNumMoleculesGlobal[s] = _nNumMoleculesLocal[s];
        _dForceSumGlobal[s] = _dForceSumLocal[s];
    }
#endif

    // calc profiles
    double dInvertShellVolume = 1. / _dShellVolume;
    double dInvertSampleTimesteps = 1. / ( (double)(_nUpdateFreq) );

	for(unsigned long s=0; s<_nNumValuesScalar; ++s)
	{
		unsigned long nNumMolecules = _nNumMoleculesGlobal[s];
		_dDensityProfile[s] = nNumMolecules * dInvertSampleTimesteps * dInvertShellVolume;

		if(nNumMolecules > 0)
			_dForceProfile[s] = _dForceSumGlobal[s] / ( (double)(nNumMolecules) );  // * dInvertSampleTimesteps; <-- wrong!?? TODO: check
		else
			_dForceProfile[s] = 0.;
	}

	// smooth profiles
	this->SmoothProfiles(_nNeighbourValsSmooth);

	// density derivation
	this->DerivateProfiles(_nNeighbourValsDerivate);
}

void DistControl::EstimateInterfaceMidpointsByForce()
{
//	for(unsigned short cid=0; cid<_nNumComponents; ++cid)
//		cout << "_nOffsets[cid] = " << _nOffsets[cid] << endl;

	unsigned int nIndexMin = 0;
	unsigned int nIndexMax = 0;
//	cout << "_nTargetCompID = " << _nTargetCompID << endl;
//	cout << "_nOffsets[_nTargetCompID] = " << _nOffsets[_nTargetCompID] << endl;
	double* dProfile = &(_dDensityProfileSmoothedDerivation[_nOffsets[_nTargetCompID] ] );
	double dMin = dProfile[0];
	double dMax = dProfile[0];

	// find min/max values and their indexes (positions) in profile
	for(unsigned int s=1; s<_nNumShells; ++s)
	{
		double dTmp = dProfile[s];
//		if(dTmp < _dVaporDensity)
//			continue;

		if(dTmp > dMax) {
			dMax = dTmp;
			nIndexMax = s;
		}

		if(dTmp < dMin) {
			dMin = dTmp;
			nIndexMin = s;
		}
	}
	_dInterfaceMidLeft  = _dMidpointPositions[nIndexMax];
	_dInterfaceMidRight = _dMidpointPositions[nIndexMin];
}

void DistControl::EstimateInterfaceMidpoint()
{

/*
    // mheinen_2015-03-17 --> DEBUG_TEST_CASE
    for(unsigned int s = 0; s < _nNumShells; ++s)
    {
        _dDensityProfile[s] = 0.7;
    }

    _dDensityProfile[0] = 0.1;
    _dDensityProfile[1] = 0.1;
    _dDensityProfile[2] = 0.1;
    _dDensityProfile[3] = 0.1;
    _dDensityProfile[4] = 0.2;
    _dDensityProfile[5] = 0.3;
    _dDensityProfile[6] = 0.4;
    _dDensityProfile[7] = 0.5;
    _dDensityProfile[8] = 0.6;

    _dDensityProfile[211] = 0.6;
    _dDensityProfile[212] = 0.5;
    _dDensityProfile[213] = 0.4;
    _dDensityProfile[214] = 0.3;
    _dDensityProfile[215] = 0.2;
    _dDensityProfile[216] = 0.1;
    _dDensityProfile[217] = 0.1;
    _dDensityProfile[218] = 0.1;
    _dDensityProfile[219] = 0.1;


    // result:
    // ----------
    // nIndexSlabGreater = 7
    // x1 = 6.5
    // dx1m = 0
    // _dInterfaceMidLeft = 6.5
    // _dInterfaceMidRight = 213.5

    // <-- DEBUG_TEST_CASE
*/


    // determine left midpoint
    {
        // determine min max
         double dMin = _dDensityProfile[0];
         double dMax = _dDensityProfile[0];

//         int nIndexMin = 0;
//         int nIndexMax = 0;

         for(unsigned int s = 0; s < _nNumShells/2; ++s)
         {
             double dDensity = _dDensityProfile[s];

             // find min
             if(dDensity < dMin)
             {
                 dMin = dDensity;
//                 nIndexMin = s;
             }

             // find max
             if(dDensity > dMax)
             {
                 dMax = dDensity;
//                 nIndexMax = s;
             }
         }

         // dN/dShellWidth = (Nsoll - Nlower) / dInterpol
         //
         // => dInterpol = (Nsoll - Nlower) / dN * dShellWidth
         //
         // dMidPos = dLeftBound + nNumSlabs_lower * dShellWidth + dInterpol

         // 1. ym bestimmen
         double ym = dMin + (dMax - dMin) / 2;

//         cout << "dMin = " << dMin << endl;
//         cout << "dMax = " << dMax << endl;
//         cout << "ym = " << ym << endl;

         // 2. Slab mit gerade niedrigerer Anzahl finden --> von links angefangen ersten Slab größer numMax finden --> Index -1 rechnen
         int nIndexSlabGreater = 0;

         for(unsigned int s = 0; s < _nNumShells; ++s)
         {
             if(_dDensityProfile[s] > ym)
             {
                 nIndexSlabGreater = s;
                 break;
             }
         }

         if(nIndexSlabGreater < 1)
         {
             cout << "ERROR in MEXRegion::EstimateInterfaceMidpoint(): could not find valid index in density profile" << endl;
             return;
         }

//         cout << "nIndexSlabGreater = " << nIndexSlabGreater << endl;

         //3. Interpolierte Distanz dInterpol berechnen, die von dem Slab mit N < Nsoll ausgehend in Richtung N > Nsoll gegangen werden muss
         double dx12, dx1m, y1, y2, dy12, dy1m;

         y1 = _dDensityProfile[nIndexSlabGreater-1];
         y2 = _dDensityProfile[nIndexSlabGreater];

         dy1m = ym - y1;

         dy12 = y2 - y1;
         dx12 = _dShellWidth;

         // dy12 / dx12 = dy1m / dx1m => dx1m = dy1m / dy12 * dx12
         dx1m = dy1m / dy12 * dx12;

         // 4. Berechnung der Mittelpunktsposition
         // nNumSlabslower == nIndexSlabLower + 1 == nIndexSlabGreater
         double xm, x1;
         double dOuterBoundarySampleZone = 0.;  //this->GetLowerCorner()[1];

         x1 = dOuterBoundarySampleZone + nIndexSlabGreater * _dShellWidth - _dShellWidth*0.5;

//         cout << "x1 = " << x1 << endl;
//         cout << "dx1m = " << dx1m << endl;

         xm = x1 + dx1m;
         _dInterfaceMidLeft = xm;

//         cout << "_dInterfaceMidLeft = " << _dInterfaceMidLeft << endl;
     }


    // determine right midpoint
    {
        // determine min max
        double dMin = _dDensityProfile[0];
        double dMax = _dDensityProfile[0];

//         int nIndexMin = 0;
//         int nIndexMax = 0;

         for(unsigned int s = _nNumShells/2; s < _nNumShells; ++s)
         {
             double dDensity = _dDensityProfile[s];

             // find min
             if(dDensity < dMin)
             {
                 dMin = dDensity;
//                 nIndexMin = s;
             }

             // find max
             if(dDensity > dMax)
             {
                 dMax = dDensity;
//                 nIndexMax = s;
             }
         }

         // dN/dShellWidth = (Nsoll - Nlower) / dInterpol
         //
         // => dInterpol = (Nsoll - Nlower) / dN * dShellWidth
         //
         // dMidPos = dLeftBound + nNumSlabs_lower * dShellWidth + dInterpol

         // 1. ym bestimmen
         double ym = dMin + (dMax - dMin) / 2;

//         cout << "ym = " << ym << endl;

         // 2. Slab mit gerade niedrigerer Anzahl finden --> von links angefangen ersten Slab größer numMax finden --> Index -1 rechnen
         unsigned int nIndexSlabGreater = 0;

         for(int s = _nNumShells-1; s >= 0; --s)
         {
             if(_dDensityProfile[s] > ym)
             {
                 nIndexSlabGreater = s;
                 break;
             }
         }

         if(nIndexSlabGreater < 1)
         {
             cout << "ERROR in MEXRegion::EstimateInterfaceMidpoint(): could not find valid index in density profile" << endl;
             return;
         }

         //3. Interpolierte Distanz dInterpol berechnen, die von dem Slab mit N < Nsoll ausgehend in Richtung N > Nsoll gegangen werden muss
         double dx12, dx1m, y1, y2, dy12, dy1m;

         y1 = _dDensityProfile[nIndexSlabGreater+1];
         y2 = _dDensityProfile[nIndexSlabGreater];

         dy1m = ym - y1;

         dy12 = y2 - y1;
         dx12 = _dShellWidth;

         // dy12 / dx12 = dy1m / dx1m => dx1m = dy1m / dy12 * dx12
         dx1m = dy1m / dy12 * dx12;

         // 4. Berechnung der Mittelpunktsposition
         // nNumSlabslower == nIndexSlabLower + 1 == nIndexSlabGreater
         Domain* domain = this->GetDomain();
         double xm, x1;
         double dOuterBoundarySampleZone = domain->getGlobalLength(1);  //this->GetUpperCorner()[1];
         unsigned int numShellsLower = _nNumShells-1 - nIndexSlabGreater;

//         cout << "numShellsLower = " << numShellsLower << endl;

         x1 = dOuterBoundarySampleZone - numShellsLower * _dShellWidth + _dShellWidth*0.5;

//         cout << "x1 = " << x1 << endl;
//         cout << "dx1m = " << dx1m << endl;

         xm = x1 - dx1m;
         _dInterfaceMidRight = xm;

//         cout << "_dInterfaceMidRight = " << _dInterfaceMidRight << endl;
    }
}


// init
void DistControl::InitPositions(double dInterfaceMidLeft, double dInterfaceMidRight)
{
    // set interface midpoints manually
    _dInterfaceMidLeft  = dInterfaceMidLeft;
    _dInterfaceMidRight = dInterfaceMidRight;
}

void DistControl::UpdatePositionsInit(ParticleContainer* particleContainer)
{
	switch(_nMethodInit )
	{
	case DCIM_START_CONFIGURATION:

		for( ParticleIterator tM  = particleContainer->iteratorBegin();
			 tM != particleContainer->iteratorEnd();
			 ++tM)
		{
			// sample density profile
			this->SampleProfiles(&(*tM));
		}

		// determine interface midpoints and update region positions
		this->UpdatePositions(this->GetUpdateFreq() );
		break;
	case DCIM_MIDPOINT_VALUES:
		break;
	case DCIM_READ_FROM_FILE:
		{
//		cout << "_strFilenameInit = " << _strFilenameInit << endl;
//		cout << "_nRestartTimestep = " << _nRestartTimestep << endl;

		ifstream filein(_strFilenameInit.c_str(), ios::in);

		string strLine, strToken;
		string strTokens[20];

		while (getline (filein, strLine))
		{
			stringstream sstr;
			sstr << strLine;

			sstr >> strToken;

			if( (unsigned long) (atoi(strToken.c_str() ) ) == _nRestartTimestep)
			{
				int i=1;
				while (sstr >> strTokens[i])
					i++;
			}
		}

		_dInterfaceMidLeft  = atof(strTokens[1].c_str() );
		_dInterfaceMidRight = atof(strTokens[2].c_str() );

		filein.close();
		break;
		}
	case DCIM_UNKNOWN:
	default:
		cout << "DistControl: Wrong Init Method! Programm exit..." << endl;
		exit(-1);
	}

#ifndef NDEBUG
	cout << "DistControl::_dInterfaceMidLeft = " << _dInterfaceMidLeft << endl;
	cout << "DistControl::_dInterfaceMidRight = " << _dInterfaceMidRight << endl;
#endif

    // update positions
    this->informObserver();

    // reset local values
    this->ResetLocalValues();
}

void DistControl::UpdatePositions(unsigned long simstep)
{
    // update with respect to update frequency
    if(simstep % _nUpdateFreq != 0 || simstep == 0 || simstep == 1)  // TODO init timestep
        return;

    // calc profiles
    this->CalcProfiles();

    // update midpoint coordinates with respect to desired method
    switch(_nMethod)
    {
    case DCUM_DENSITY_PROFILE:
		this->EstimateInterfaceMidpoint();
		break;
    case DCUM_DENSITY_PROFILE_DERIVATION:
    	this->EstimateInterfaceMidpointsByForce();
    	break;
    case DCUM_UNKNOWN:
    default:
		cout << "DistControl::UpdatePositions: Corrupted code!!! Programm exit..." << endl;
    	exit(-1);
    }

    // update positions
    this->informObserver();

    // reset local values
    this->ResetLocalValues();
}


void DistControl::ResetLocalValues()
{
	for(unsigned int s = 0; s < _nNumValuesScalar; ++s)
	{
		_nNumMoleculesLocal[s] = 0;
		_dForceSumLocal[s] = 0.;
	}
}

void DistControl::ResetLocalValuesCOM()
{
	for(uint8_t c=0; c<_nNumComponents; ++c)
	{
		_nNumMoleculesCOMLocal[c] = 0;
		for(uint8_t d=0; d<3; ++d)
			_dPosSumLocal[c+d] = 0.;
	}
}

void DistControl::WriteData(unsigned long simstep)
{
	// domain decomposition
	DomainDecompBase* domainDecomp = this->GetDomainDecomposition();

    // write out data
    stringstream outputstream;

    // write data
    if(simstep % _nWriteFreq == 0)
    {
        // calc global values
//        this->CalcGlobalValues(domainDecomp);

#ifdef ENABLE_MPI
    int rank = domainDecomp->getRank();
    // int numprocs = domainDecomp->getNumProcs();
    if (rank == 0)
    {
#endif
        outputstream << std::setw(20) << simstep;
        outputstream << std::setw(24) << std::scientific << std::setprecision(std::numeric_limits<double>::digits10) << _dInterfaceMidLeft;
        outputstream << std::setw(24) << std::scientific << std::setprecision(std::numeric_limits<double>::digits10) << _dInterfaceMidRight;

        // observer data
    	std::vector<ObserverBase*>::iterator it;

    	for(it=_observer.begin(); it!=_observer.end(); it++)
    	{
    		CuboidRegionObs* region = static_cast<CuboidRegionObs*>(*it);
    		if(NULL != region)
    		{
    	        outputstream << std::setw(24) << std::scientific << std::setprecision(std::numeric_limits<double>::digits10) << region->GetLowerCorner(1);
    	        outputstream << std::setw(24) << std::scientific << std::setprecision(std::numeric_limits<double>::digits10) << region->GetUpperCorner(1);
    		}
    	}

        outputstream << endl;

        ofstream fileout(_strFilename.c_str(), ios::out|ios::app);
        fileout << outputstream.str();
        fileout.close();
#ifdef ENABLE_MPI
    }
#endif
    }
}

void DistControl::WriteHeader()
{
	// domain decomposition
	DomainDecompBase* domainDecomp = this->GetDomainDecomposition();

    // write header
    stringstream outputstream;

#ifdef ENABLE_MPI
    int rank = domainDecomp->getRank();
    // int numprocs = domainDecomp->getNumProcs();
    if (rank == 0)
    {
#endif

    outputstream << "             simstep";
    outputstream << "                 midLeft";
    outputstream << "                midRight";

    // observer data
	std::vector<ObserverBase*>::iterator it;

	for(it=_observer.begin(); it!=_observer.end(); it++)
	{
		CuboidRegionObs* region = static_cast<CuboidRegionObs*>(*it);
		if(NULL != region)
		{
			outputstream << "                 " << region->GetParent()->GetShortName() << "l" << "[" << region->GetID() << "]";
			outputstream << "                 " << region->GetParent()->GetShortName() << "r" << "[" << region->GetID() << "]";
		}
	}

    outputstream << endl;

    ofstream fileout(_strFilename.c_str(), ios::out);
    fileout << outputstream.str();
    fileout.close();

#ifdef ENABLE_MPI
    }
#endif
}


void DistControl::WriteDataProfiles(unsigned long simstep)
{
	if(simstep % _nWriteFreqProfiles != 0)
		return;

	// domain decomposition
	DomainDecompBase* domainDecomp = this->GetDomainDecomposition();

    stringstream outputstream;
    stringstream filenamestream;

    filenamestream << _strFilenameProfilesPrefix << "_TS";
    filenamestream.fill('0');
    filenamestream.width(9);
    filenamestream << right << simstep;
    filenamestream << ".dat";

#ifdef ENABLE_MPI
    int rank = domainDecomp->getRank();
    // int numprocs = domainDecomp->getNumProcs();
    if (rank != 0)
		return;
#endif

	// write header
	outputstream << "                   coord";
	for(unsigned short cid=0; cid<_nNumComponents; ++cid)
	{
		outputstream << "                  rho[" << cid << "]";
		outputstream << "           rho_smooth[" << cid << "]";
		outputstream << "              drho/dy[" << cid << "]";
		outputstream << "                   Fy[" << cid << "]";
		outputstream << "            Fy_smooth[" << cid << "]";
	}
	outputstream << endl;
	// write data
	for(unsigned int s=0; s<_nNumShells; ++s)
	{
		outputstream << FORMAT_SCI_MAX_DIGITS << _dMidpointPositions[s];
		for(unsigned short cid=0; cid<_nNumComponents; ++cid)
		{
			unsigned long nIndex = _nOffsets[cid]+s;
			outputstream << FORMAT_SCI_MAX_DIGITS << _dDensityProfile[nIndex];
			outputstream << FORMAT_SCI_MAX_DIGITS << _dDensityProfileSmoothed[nIndex];
			outputstream << FORMAT_SCI_MAX_DIGITS << _dDensityProfileSmoothedDerivation[nIndex];
			outputstream << FORMAT_SCI_MAX_DIGITS << _dForceProfile[nIndex];
			outputstream << FORMAT_SCI_MAX_DIGITS << _dForceProfileSmoothed[nIndex];
		}
		outputstream << endl;
	}
	ofstream fileout(filenamestream.str().c_str(), ios::out|ios::out);
	fileout << outputstream.str();
	fileout.close();
}


void DistControl::Init(ParticleContainer* particleContainer)
{
	this->InitDataStructurePointers();
	this->PrepareSubdivision();
	this->PrepareDataStructures();
}


void DistControl::AlignSystemCenterOfMass(Molecule* mol, unsigned long simstep)
{
	// domain
	Domain* domain = this->GetDomain();

    // update with respect to update frequency
    if(simstep % _nUpdateFreq != 0 || simstep == 0 || simstep == 1)  // TODO init timestep
        return;

    double dDeltaLeftY  = _dInterfaceMidLeft;  // (minus 0)
    double dDeltaRightY = domain->getGlobalLength(1) - _dInterfaceMidRight;
    double dDeltaY = 0.5 * (dDeltaRightY - dDeltaLeftY);

    double dNewPosition = mol->r(1) + dDeltaY;

/*
 * seems not to work: simulation crashes!!!
 *
    double dBoxLengthY = domain->getGlobalLength(1);

    if (dNewPosition > dBoxLengthY )
        dNewPosition -= dBoxLengthY;
    else if (dNewPosition < 0. )
        dNewPosition += dBoxLengthY;
*/

    mol->setr(1, dNewPosition);

#ifdef DEBUG
    cout << "_dInterfaceMidLeft = " << _dInterfaceMidLeft << endl;
    cout << "_dInterfaceMidRight = " << _dInterfaceMidRight << endl;
    cout << "dDeltaLeftY = " << dDeltaLeftY << endl;
    cout << "dDeltaRightY = " << dDeltaRightY << endl;
    cout << "dDeltaY = " << dDeltaY << endl;
#endif
}

void DistControl::SampleCOM(Molecule* mol, unsigned long simstep)
{
	uint8_t cid = mol->componentid() + 1;  // cid == 0: all components

	_nNumMoleculesCOMLocal[cid]++;
	for(uint8_t d=0; d<3; ++d)
		_dPosSumLocal[cid+d] += mol->r(d);
}

void DistControl::CalcGlobalValuesCOM(unsigned long simstep)
{
#ifdef ENABLE_MPI

	MPI_Allreduce( _nNumMoleculesCOMLocal, _nNumMoleculesCOMGlobal, _nNumComponents, MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
	MPI_Allreduce( _dPosSumLocal, _dPosSumGlobal, (_nNumComponents*3), MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

#else
	for(uint8_t c=0; c<_nNumComponents; ++c)
	{
		_nNumMoleculesCOMGlobal[c] = _nNumMoleculesCOMLocal[c];
		for(uint8_t d=0; d<3; ++d)
			_dPosSumGlobal[c+d] = _dPosSumLocal[c+d];
	}
#endif
	double dCuttOffQuarter = global_simulation->getcutoffRadius() * 0.25;
	double dInvNumMolecules = 1. / (double)(_nNumMoleculesCOMGlobal[_nTargetCompID]);
	for(uint8_t d=0; d<3; ++d)
	{
		_dPosCOM[d] = _dPosSumGlobal[_nTargetCompID+d] * dInvNumMolecules;
		double dDelta = _dPosTargetCOM[d] - _dPosCOM[d];
		int nSign = (dDelta > 0.) ? 1 : -1;
		dDelta = (abs(dDelta) < dCuttOffQuarter) ? dDelta : dCuttOffQuarter * nSign;
		_dAddVec[d] = dDelta;
	}

#ifndef NDEBUG
	cout << "DistControl::_dPosCOM = " << _dPosCOM[0] << ", " << _dPosCOM[1] << ", " << _dPosCOM[2] << endl;
	cout << "DistControl::_dPosTargetCOM = " << _dPosTargetCOM[0] << ", " << _dPosTargetCOM[1] << ", " << _dPosTargetCOM[2] << endl;
	cout << "DistControl::_dAddVec = " << _dAddVec[0] << ", " << _dAddVec[1] << ", " << _dAddVec[2] << endl;
#endif

	this->ResetLocalValuesCOM();
}

void DistControl::AlignCOM(Molecule* mol, unsigned long simstep)
{
	for(uint8_t d=0; d<3; ++d)
		mol->setr(d, mol->r(d)+_dAddVec[d]);
}

void DistControl::registerObserver(ObserverBase* observer)
{
	_observer.push_back(observer);
}

void DistControl::deregisterObserver(ObserverBase* observer)
{
	std::vector<ObserverBase*>::iterator it;

	for(it=_observer.begin(); it!=_observer.end(); it++)
	{
		if( (*it) == observer)
		{
			_observer.erase(it);
		}
	}
}

void DistControl::informObserver()
{
	std::vector<ObserverBase*>::iterator it;

	for(it=_observer.begin(); it!=_observer.end(); it++)
	{
		(*it)->set(_dInterfaceMidLeft, _dInterfaceMidRight);
	}
}

void DistControl::SmoothProfile(double* dData, double* dSmoothData, const unsigned long& nNumVals, const unsigned int& nNeighbourVals)
{
	unsigned int li = 0;
	unsigned int ri = nNeighbourVals;

	for(unsigned int s=0; s<nNumVals; ++s)
	{
		double dSum = 0.;
		unsigned short nNumValsSmooth = 0;
		for(unsigned short ti = s-li; ti <= s+ri; ++ti) {
			dSum += dData[ti];
			nNumValsSmooth++;
		}
		double dInvNumValsSmooth = 1. / ( (double)(nNumValsSmooth) );
		dSmoothData[s] = dSum * dInvNumValsSmooth;

		if(li < nNeighbourVals)
			li++;

		if(s >= (nNumVals-1 - nNeighbourVals) )
			ri--;
	}
}

void DistControl::SmoothProfiles(const unsigned int& nNeighbourVals)
{
	for(unsigned short cid=0; cid<_nNumComponents; ++cid)
	{
		unsigned long nOffset = _nOffsets[cid];
		// smooth density profiles
		SmoothProfile( &(_dDensityProfile[nOffset] ), &(_dDensityProfileSmoothed[nOffset] ),
				_nNumShells, nNeighbourVals);
		// smooth force profiles
		SmoothProfile( &(_dForceProfile[nOffset] ), &(_dForceProfileSmoothed[nOffset] ),
				_nNumShells, nNeighbourVals);
	}
}

void DistControl::DerivateProfile(double* dDataX, double* dDataY, double* dDerivDataY, const unsigned long& nNumVals, const unsigned int& nNeighbourVals)
{
    unsigned int li = 0;
    unsigned int ri = nNeighbourVals;

/*
 * derivate by two points
 *
    for(unsigned int s = 0; s < _nNumShells; ++s)
    {
        double dy = _dDensityProfileSmoothed[s+r] - _dDensityProfileSmoothed[s-l];
        double dx = (r+l)*_dShellWidth;

        _dDensityProfileSmoothedDerivation[s] = dy / dx;

        if(l < nNumNeighbourVals)
            l++;

        if(s >= (_nNumShells-1 - nNumNeighbourVals) )
            r--;
    }
*/

	vector<double> x;
	vector<double> y;

	for(unsigned int s = 0; s < nNumVals; ++s)
    {
		x.clear();
		y.clear();

		for(unsigned int i = s-li; i<=s+ri; ++i)
    	{
			x.push_back(dDataX[i] );
			y.push_back(dDataY[i] );
    	}
		double beta1, beta2;
        LinearRegression(x, y, beta1, beta2);

        dDerivDataY[s] = beta2;

        if(li < nNeighbourVals)
            li++;

        if(s >= (_nNumShells-1 - nNeighbourVals) )
            ri--;
    }
}

void DistControl::DerivateProfiles(const unsigned int& nNeighbourVals)
{
	for(unsigned short cid=0; cid<_nNumComponents; ++cid)
	{
		unsigned long nOffset = _nOffsets[cid];
		DerivateProfile(_dMidpointPositions, &(_dDensityProfileSmoothed[nOffset] ), &(_dDensityProfileSmoothedDerivation[nOffset] ),
				_nNumShells, nNeighbourVals);
	}
}









