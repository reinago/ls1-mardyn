/*
 * TemperatureControl.cpp
 *
 *  Created on: 27.05.2015
 *      Author: mheinen
 */

#include "thermostats/TemperatureControl.h"
#include "particleContainer/ParticleContainer.h"
#include "parallel/DomainDecompBase.h"
#include "molecules/Molecule.h"
#include "Domain.h"
#include "utils/xmlfileUnits.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>

using namespace std;

// init static ID --> instance counting
unsigned short ControlRegionT::_nStaticID = 0;

// class ControlRegionT

ControlRegionT::ControlRegionT()
	:
	_nID(0),
	_dLowerCorner{0.,0.,0.},
	_dUpperCorner{0.,0.,0.},
	_nNumSlabs(0),
	_dSlabWidth(0.0),
	_nNumMoleculesLocal(nullptr),
	_nNumMoleculesGlobal(nullptr),
	_nRotDOFLocal(nullptr),
	_nRotDOFGlobal(nullptr),
	_d2EkinTransLocal(nullptr),
	_d2EkinTransGlobal(nullptr),
	_d2EkinRotLocal(nullptr),
	_d2EkinRotGlobal(nullptr),
	_dBetaTransGlobal(nullptr),
	_dBetaRotGlobal(nullptr),
	_dTargetTemperature(0.0),
	_dTemperatureExponent(0.0),
	_nTargetComponentID(0),
	_nNumThermostatedTransDirections(0),
	_nRegionID(0),
	_accumulator(nullptr),
	_strFilenamePrefixBetaLog("beta_log"),
	_nWriteFreqBeta(1000),
	_numSampledConfigs(0),
	_dBetaTransSumGlobal(0.0),
	_dBetaRotSumGlobal(0.0)
{
	// ID
	_nID = ++_nStaticID;
}


ControlRegionT::~ControlRegionT()
{
}

AccumulatorBase* ControlRegionT::CreateAccumulatorInstance(std::string strTransDirections)
{
	AccumulatorBase* accumulator;

	if(strTransDirections == "x")
	{
		accumulator = new AccumulatorX();
		_nNumThermostatedTransDirections = 1;
	}
	else if(strTransDirections == "y")
	{
		accumulator = new AccumulatorY();
		_nNumThermostatedTransDirections = 1;
	}
	else if(strTransDirections == "z")
	{
		accumulator = new AccumulatorZ();
		_nNumThermostatedTransDirections = 1;
	}
	else if(strTransDirections == "xy")
	{
		accumulator = new AccumulatorXY();
		_nNumThermostatedTransDirections = 2;
	}
	else if(strTransDirections == "xz")
	{
		accumulator = new AccumulatorXZ();
		_nNumThermostatedTransDirections = 2;
	}
	else if(strTransDirections == "yz")
	{
		accumulator = new AccumulatorYZ();
		_nNumThermostatedTransDirections = 2;
	}
	else if(strTransDirections == "xyz")
	{
		accumulator = new AccumulatorXYZ();
		_nNumThermostatedTransDirections = 3;
	}
	else
		accumulator = NULL;

	return accumulator;
}

void ControlRegionT::readXML(XMLfileUnits& xmlconfig)
{
	Domain* domain = global_simulation->getDomain();
	double lc[3];
	double uc[3];
	std::string strVal[3];
	std::string strDirections;

	// coordinates
	xmlconfig.getNodeValue("coords/lcx", lc[0]);
	xmlconfig.getNodeValue("coords/lcy", lc[1]);
	xmlconfig.getNodeValue("coords/lcz", lc[2]);
	xmlconfig.getNodeValue("coords/ucx", strVal[0]);
	xmlconfig.getNodeValue("coords/ucy", strVal[1]);
	xmlconfig.getNodeValue("coords/ucz", strVal[2]);
	// read upper corner
	for(uint8_t d=0; d<3; ++d)
		uc[d] = (strVal[d] == "box") ? domain->getGlobalLength(d) : atof(strVal[d].c_str() );

#ifndef NDEBUG
	global_log->info() << "TemperatureControl: upper corner: " << uc[0] << ", " << uc[1] << ", " << uc[2] << endl;
#endif

	for(uint8_t d=0; d<3; ++d)
	{
		_dLowerCorner[d] = lc[d];
		_dUpperCorner[d] = uc[d];
	}

	// target values
	xmlconfig.getNodeValue("target/temperature", _dTargetTemperature);
	xmlconfig.getNodeValue("target/component", _nTargetComponentID);

	// settings
	xmlconfig.getNodeValue("settings/numslabs", _nNumSlabs);
	xmlconfig.getNodeValue("settings/exponent", _dTemperatureExponent);
	xmlconfig.getNodeValue("settings/directions", strDirections);
	// calc slab width
	_dSlabWidth = this->GetWidth(1) / ( (double)(_nNumSlabs) );
	// create accumulator instance
	_accumulator = this->CreateAccumulatorInstance(strDirections);

	// write control for beta_trans and beta_rot log file
	_nWriteFreqBeta = 1000;
	_strFilenamePrefixBetaLog = "beta_log";
	xmlconfig.getNodeValue("writefreq",  _nWriteFreqBeta);
	xmlconfig.getNodeValue("fileprefix", _strFilenamePrefixBetaLog);
	if(_nWriteFreqBeta==0) {
		global_log->warning() << "Temperature Control: write Frequency was specified to be zero. This is NOT allowed. Reset it to 1000." << std::endl;
		_nWriteFreqBeta = 1000;
	}
	this->InitBetaLogfile();

	// init data structures
	this->Init();
}

void ControlRegionT::Init()
{
	_nNumMoleculesLocal  = new unsigned long[_nNumSlabs];
	_nNumMoleculesGlobal = new unsigned long[_nNumSlabs];
	_nRotDOFLocal  = new unsigned long[_nNumSlabs];
	_nRotDOFGlobal = new unsigned long[_nNumSlabs];

	_d2EkinTransLocal  = new double[_nNumSlabs];
	_d2EkinTransGlobal = new double[_nNumSlabs];
	_d2EkinRotLocal  = new double[_nNumSlabs];
	_d2EkinRotGlobal = new double[_nNumSlabs];

	_dBetaTransGlobal = new double[_nNumSlabs];
	_dBetaRotGlobal   = new double[_nNumSlabs];

	for(unsigned int s = 0; s<_nNumSlabs; ++s)
	{
		_nNumMoleculesLocal[s]  = 0;
		_nNumMoleculesGlobal[s] = 0;
		_nRotDOFLocal[s]  = 0;
		_nRotDOFGlobal[s] = 0;

		_d2EkinTransLocal[s]  = 0.;
		_d2EkinTransGlobal[s] = 0.;
		_d2EkinRotLocal[s]  = 0.;
		_d2EkinRotGlobal[s] = 0.;

		_dBetaTransGlobal[s] = 0.;
		_dBetaRotGlobal[s]   = 0.;
	}
}

void ControlRegionT::CalcGlobalValues(DomainDecompBase* /*domainDecomp*/)
{
#ifdef ENABLE_MPI

	MPI_Allreduce( _nNumMoleculesLocal, _nNumMoleculesGlobal, _nNumSlabs, MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);
	MPI_Allreduce( _nRotDOFLocal, _nRotDOFGlobal, _nNumSlabs, MPI_UNSIGNED_LONG, MPI_SUM, MPI_COMM_WORLD);

	MPI_Allreduce( _d2EkinTransLocal, _d2EkinTransGlobal, _nNumSlabs, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
	MPI_Allreduce( _d2EkinRotLocal, _d2EkinRotGlobal, _nNumSlabs, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

#else
	for(unsigned int s = 0; s<_nNumSlabs; ++s)
	{
		_nNumMoleculesGlobal[s] = _nNumMoleculesLocal[s];
		_nRotDOFGlobal[s] = _nRotDOFLocal[s];

		_d2EkinTransGlobal[s] = _d2EkinTransLocal[s];
		_d2EkinRotGlobal[s] = _d2EkinRotLocal[s];
	}
#endif

	// calc betaTrans, betaRot, and their sum
	double dBetaTransSumSlabs = 0.;
	double dBetaRotSumSlabs = 0.;

	for(unsigned int s = 0; s<_nNumSlabs; ++s)
	{
		if( _nNumMoleculesGlobal[s] < 1 )
			_dBetaTransGlobal[s] = 1.;
		else
			_dBetaTransGlobal[s] = pow(_nNumThermostatedTransDirections * _nNumMoleculesGlobal[s] * _dTargetTemperature / _d2EkinTransGlobal[s], _dTemperatureExponent);

		if( _nRotDOFGlobal[s] < 1 )
			_dBetaRotGlobal[s] = 1.;
		else
			_dBetaRotGlobal[s] = pow( _nRotDOFGlobal[s] * _dTargetTemperature / _d2EkinRotGlobal[s], _dTemperatureExponent);

		// calc sums over all slabs
		dBetaTransSumSlabs += _dBetaTransGlobal[s];
		dBetaRotSumSlabs   += _dBetaRotGlobal[s];
	}
	// calc ensemble average of beta_trans, beta_rot
	_dBetaTransSumGlobal += dBetaTransSumSlabs;
	_dBetaRotSumGlobal   += dBetaRotSumSlabs;
	_numSampledConfigs++;

//    cout << "_nNumMoleculesGlobal = " << _nNumMoleculesGlobal << endl;
//    cout << "_dBetaTransGlobal = " << _dBetaTransGlobal << endl;
//    cout << "_dTargetTemperature = " << _dTargetTemperature << endl;
//    cout << "_d2EkinRotGlobal = " << _d2EkinRotGlobal << endl;
//
//    cout << "_nRotDOFGlobal = " << _nRotDOFGlobal << endl;
//    cout << "_dBetaRotGlobal = " << _dBetaRotGlobal << endl;
//    cout << "_d2EkinRotGlobal = " << _d2EkinRotGlobal << endl;

}

void ControlRegionT::MeasureKineticEnergy(Molecule* mol, DomainDecompBase* /*domainDecomp*/)
{
	// check componentID
	if(mol->componentid()+1 != _nTargetComponentID && 0 != _nTargetComponentID)  // program intern componentID starts with 0
		return;

	// check if molecule inside control region
	for(unsigned short d = 0; d<3; ++d)
	{
		double dPos = mol->r(d);

		if(dPos <= _dLowerCorner[d] || dPos >= _dUpperCorner[d] )
			return;
	}

	unsigned int nPosIndex;
	unsigned int nIndexMax = _nNumSlabs - 1;

	// calc position index
	double* dLowerCorner = this->GetLowerCorner();
	double dPosRelative = mol->r(1) - dLowerCorner[1];

	nPosIndex = (unsigned int) floor(dPosRelative / _dSlabWidth);

	// ignore outer (halo) molecules
	if(nPosIndex > nIndexMax)  // negative values will be ignored to: cast to unsigned int --> high value
		return;

	// sum up transl. kinetic energy (2x)
/*
	double vx = mol->v(0);
//    double vy = mol->v(1);
	double vz = mol->v(2);
	double m  = mol->mass();

//    _d2EkinTransLocal += m*(vx*vx + vy*vy);
	_d2EkinTransLocal[nPosIndex] += m*(vx*vx + vz*vz);
*/

	_d2EkinTransLocal[nPosIndex] += _accumulator->CalcKineticEnergyContribution(mol);

	// sum up rot. kinetic energy (2x)
	double dDummy = 0.;

	mol->calculate_mv2_Iw2(dDummy, _d2EkinRotLocal[nPosIndex] );

	// count num molecules
	_nNumMoleculesLocal[nPosIndex]++;

	// count rotational DOF
	_nRotDOFLocal[nPosIndex] += mol->component()->getRotationalDegreesOfFreedom();
}


void ControlRegionT::ControlTemperature(Molecule* mol)
{
	// check componentID
	if(mol->componentid()+1 != _nTargetComponentID && 0 != _nTargetComponentID)  // program intern componentID starts with 0
		return;

	// check if molecule is inside
	for(unsigned short d = 0; d<3; ++d)
	{
		double dPos = mol->r(d);

		if(dPos <= _dLowerCorner[d] || dPos >= _dUpperCorner[d] )
			return;
	}

	unsigned int nPosIndex;
	unsigned int nIndexMax = _nNumSlabs - 1;

	// calc position index
	double* dLowerCorner = this->GetLowerCorner();
	double dPosRelative = mol->r(1) - dLowerCorner[1];

	nPosIndex = (unsigned int) floor(dPosRelative / _dSlabWidth);

	// ignore outer (halo) molecules
	if(nPosIndex > nIndexMax)  // negative values will be ignored to: cast to unsigned int --> high value
		return;

	if(_nNumMoleculesGlobal[nPosIndex] < 1)
		return;


	// scale velocity
	double vcorr = 2. - 1. / _dBetaTransGlobal[nPosIndex];
	double Dcorr = 2. - 1. / _dBetaRotGlobal[nPosIndex];

/*
	mol->setv(0, mol->v(0) * vcorr);
//    mol->setv(1, mol->v(1) * vcorr);
	mol->setv(2, mol->v(2) * vcorr);
*/

	_accumulator->ScaleVelocityComponents(mol, vcorr);

	mol->scale_D(Dcorr);
}

void ControlRegionT::ResetLocalValues()
{
	// reset local values
	for(unsigned int s = 0; s<_nNumSlabs; ++s)
	{
		_nNumMoleculesLocal[s] = 0;
		_nRotDOFLocal[s] = 0;

		_d2EkinTransLocal[s] = 0.;
		_d2EkinRotLocal[s] = 0.;

		_dBetaTransGlobal[s] = 1.;
		_dBetaRotGlobal[s] = 1.;
	}
}

void ControlRegionT::InitBetaLogfile()
{
	DomainDecompBase* domainDecomp = &(global_simulation->domainDecomposition() );

#ifdef ENABLE_MPI
	int rank = domainDecomp->getRank();
	// int numprocs = domainDecomp->getNumProcs();
	if (rank!= 0)
		return;
#endif

	std::stringstream filenamestream;
	filenamestream << _strFilenamePrefixBetaLog << "_reg" << this->GetID() << ".dat";
	std::stringstream outputstream;
	outputstream.write(reinterpret_cast<const char*>(&_nWriteFreqBeta), 8);

	ofstream fileout(filenamestream.str().c_str(), std::ios::out | std::ios::binary);
	fileout << outputstream.str();
	fileout.close();
}

void ControlRegionT::WriteBetaLogfile(unsigned long simstep)
{
	if(0 != (simstep % _nWriteFreqBeta) )
		return;

	DomainDecompBase* domainDecomp = &(global_simulation->domainDecomposition() );

#ifdef ENABLE_MPI
	int rank = domainDecomp->getRank();
	// int numprocs = domainDecomp->getNumProcs();
	if (rank!= 0)
		return;
#endif

	std::stringstream filenamestream;
	filenamestream << _strFilenamePrefixBetaLog << "_reg" << this->GetID() << ".dat";
	std::stringstream outputstream;
	double dInvNumConfigs = 1. / (double)(_numSampledConfigs);
	double dBetaTrans = _dBetaTransSumGlobal * dInvNumConfigs;
	double dBetaRot   = _dBetaRotSumGlobal   * dInvNumConfigs;
	outputstream.write(reinterpret_cast<const char*>(&dBetaTrans), 8);
	outputstream.write(reinterpret_cast<const char*>(&dBetaRot),   8);

	ofstream fileout(filenamestream.str().c_str(), std::ios::app | std::ios::binary);
	fileout << outputstream.str();
	fileout.close();

	// reset averaged values
	_numSampledConfigs = 0;
	_dBetaTransSumGlobal = 0.;
	_dBetaRotSumGlobal = 0.;
}

// class TemperatureControl
TemperatureControl::TemperatureControl()
{
}

TemperatureControl::~TemperatureControl()
{

}

void TemperatureControl::readXML(XMLfileUnits& xmlconfig)
{
	// control
	xmlconfig.getNodeValue("control/start", _nStart);
	xmlconfig.getNodeValue("control/frequency", _nControlFreq);
	xmlconfig.getNodeValue("control/stop", _nStop);
	global_log->info() << "Start control from simstep: " << _nStart << endl;
	global_log->info() << "Control with frequency: " << _nControlFreq << endl;
	global_log->info() << "Stop control at simstep: " << _nStop << endl;

	// turn on/off explosion heuristics
	// domain->SetExplosionHeuristics(bUseExplosionHeuristics);

	// add regions
	uint32_t numRegions = 0;
	XMLfile::Query query = xmlconfig.query("regions/region");
	numRegions = query.card();
	global_log->info() << "Number of control regions: " << numRegions << endl;
	if(numRegions < 1) {
		global_log->warning() << "No region parameters specified." << endl;
	}
	string oldpath = xmlconfig.getcurrentnodepath();
	XMLfile::Query::const_iterator outputRegionIter;
	for( outputRegionIter = query.begin(); outputRegionIter; outputRegionIter++ )
	{
		xmlconfig.changecurrentnode(outputRegionIter);
		ControlRegionT* region = new ControlRegionT();
		region->readXML(xmlconfig);
		this->AddRegion(region);
	}
}

void TemperatureControl::AddRegion(ControlRegionT* region)
{
	_vecControlRegions.push_back(region);
}

void TemperatureControl::MeasureKineticEnergy(Molecule* mol, DomainDecompBase* domainDecomp, unsigned long simstep)
{
	if(simstep % _nControlFreq != 0)
		return;

	for(auto&& reg : _vecControlRegions)
		reg->MeasureKineticEnergy(mol, domainDecomp);
}

void TemperatureControl::CalcGlobalValues(DomainDecompBase* domainDecomp, unsigned long simstep)
{
	if(simstep % _nControlFreq != 0)
		return;

	for(auto&& reg : _vecControlRegions)
		reg->CalcGlobalValues(domainDecomp);
}


void TemperatureControl::ControlTemperature(Molecule* mol, unsigned long simstep)
{
	if(simstep % _nControlFreq != 0)
		return;

	for(auto&& reg : _vecControlRegions)
		reg->ControlTemperature(mol);
}

void TemperatureControl::Init(unsigned long simstep)
{
	if(simstep % _nControlFreq != 0)
		return;

	for(auto&& reg : _vecControlRegions)
		reg->ResetLocalValues();
}

void TemperatureControl::InitBetaLogfiles()
{
	for(auto&& reg : _vecControlRegions)
		reg->InitBetaLogfile();
}

void TemperatureControl::WriteBetaLogfiles(unsigned long simstep)
{
	for(auto&& reg : _vecControlRegions)
		reg->WriteBetaLogfile(simstep);
}

void TemperatureControl::DoLoopsOverMolecules(DomainDecompBase* domainDecomposition, ParticleContainer* particleContainer, unsigned long simstep)
{
	// respect start/stop
	if(this->GetStart() <= simstep && this->GetStop() > simstep)
	{
//		global_log->info() << "Thermostat ON!" << endl;

		ParticleIterator tM;

		// init temperature control
		this->Init(simstep);

		for( tM  = particleContainer->iteratorBegin();
			 tM != particleContainer->iteratorEnd();
			 ++tM)
		{
			// measure kinetic energy
			this->MeasureKineticEnergy(&(*tM), domainDecomposition, simstep);

//          cout << "id = " << tM->id() << ", (vx,vy,vz) = " << tM->v(0) << ", " << tM->v(1) << ", " << tM->v(2) << endl;
		}

		// calc global values
		this->CalcGlobalValues(domainDecomposition, simstep);

		// write beta_trans, beta_rot log-files
		this->WriteBetaLogfiles(simstep);

		for( tM  = particleContainer->iteratorBegin();
			 tM != particleContainer->iteratorEnd();
			 ++tM)
		{
			// control temperature
			this->ControlTemperature(&(*tM), simstep);

//          cout << "id = " << tM->id() << ", (vx,vy,vz) = " << tM->v(0) << ", " << tM->v(1) << ", " << tM->v(2) << endl;
		}
	}
}


