
#define SIMULATION_SRC
#include "Simulation.h"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>

#include "WrapOpenMP.h"

#include "Common.h"
#include "Domain.h"
#include "particleContainer/LinkedCells.h"
#include "parallel/DomainDecompBase.h"
#include "parallel/NonBlockingMPIHandlerBase.h"
#include "parallel/NonBlockingMPIMultiStepHandler.h"
#include "molecules/Molecule.h"

#ifdef ENABLE_MPI
#include "parallel/DomainDecomposition.h"
#include "parallel/KDDecomposition.h"
#endif

#include "particleContainer/adapter/ParticlePairs2PotForceAdapter.h"
#include "particleContainer/adapter/LegacyCellProcessor.h"
#include "particleContainer/adapter/VectorizedCellProcessor.h"
#include "particleContainer/adapter/VCP1CLJWR.h"
#include "particleContainer/adapter/FlopCounter.h"
#include "integrators/Integrator.h"
#include "integrators/Leapfrog.h"
#include "integrators/ExplicitEuler.h"
#include "molecules/Wall.h"
#include "molecules/Mirror.h"

#include "io/io.h"
#include "io/GeneratorFactory.h"
#include "io/RDF.h"
#include "io/TcTS.h"
#include "io/Mkesfera.h"
#include "io/TimerProfiler.h"
#include "io/MemoryProfiler.h"

#include "ensemble/GrandCanonical.h"
#include "ensemble/CanonicalEnsemble.h"
#include "ensemble/PressureGradient.h"
#include "ensemble/CavityEnsemble.h"

#include "thermostats/VelocityScalingThermostat.h"
#include "thermostats/TemperatureControl.h"

#include "utils/FileUtils.h"
#include "utils/OptionParser.h"
#include "utils/Logger.h"

#include "longRange/LongRangeCorrection.h"
#include "longRange/Homogeneous.h"
#include "longRange/Planar.h"

#include "bhfmm/FastMultipoleMethod.h"
#include "bhfmm/cellProcessors/VectorizedLJP2PCellProcessor.h"

#include "particleContainer/adapter/VectorizationTuner.h"

#include "NEMD/NEMD.h"
#include "NEMD/DriftControl.h"
#include "NEMD/DistControl.h"
#include "NEMD/RegionSampling.h"
#include "NEMD/DensityControl.h"
#include "NEMD/ParticleTracker.h"

using Log::global_log;
using optparse::OptionParser;
using optparse::OptionGroup;
using optparse::Values;
using namespace std;


Simulation* global_simulation;

Simulation::Simulation()
	:
	_simulationTime(0),
	_initSimulation(0),
	_initCanonical(0),
	_initGrandCanonical(0),
	_initStatistics(0),
	_ensemble(NULL),
	_rdf(NULL),
	_moleculeContainer(NULL),
	_particlePairsHandler(NULL),
	_cellProcessor(NULL),
	_flopCounter(NULL),
	_domainDecomposition(nullptr),
	_integrator(NULL),
	_domain(NULL),
	_inputReader(NULL),
	_longRangeCorrection(NULL),
	_temperatureControl(NULL),
	_FMM(NULL),
	_timerProfiler(),
	_forced_checkpoint_time(0),
	_loopCompTime(0.),
	_loopCompTimeSteps(0),
	_programName(""),
	_flagsNEMD(0)
{
	_ensemble = new CanonicalEnsemble();
	_memoryProfiler = new MemoryProfiler();
	_memoryProfiler->registerObject(reinterpret_cast<MemoryProfilable**>(&_moleculeContainer));

    // NEMD features
    _driftControl = NULL;
    _distControl = NULL;
    _densityControl = NULL;
    _regionSampling = NULL;
    _particleTracker = NULL;

	initialize();
}

Simulation::~Simulation() {
	delete _domainDecomposition;
	delete _pressureGradient;
	delete _domain;
	delete _particlePairsHandler;
	delete _cellProcessor;
	delete _moleculeContainer;
	delete _integrator;
	delete _inputReader;
	delete _flopCounter;
	delete _FMM;
	delete _ensemble;
	delete _longRangeCorrection;
}

void Simulation::exit(int exitcode) {
	// .. to avoid code duplication ..
	mardyn_exit(exitcode);
}

void Simulation::readXML(XMLfileUnits& xmlconfig) {
#ifdef USE_VT
	VT_traceoff();
#endif
	/* integrator */
	if(xmlconfig.changecurrentnode("integrator")) {
		string integratorType;
		xmlconfig.getNodeValue("@type", integratorType);
		global_log->info() << "Integrator type: " << integratorType << endl;
		if(integratorType == "Leapfrog") {
			_integrator = new Leapfrog();
		} else if (integratorType == "ExplicitEuler") {
			global_log->info() << "Integrator type: Explicit Euler (WR mode only)" << endl;
			_integrator = new ExplicitEuler();
		} else {
			global_log-> error() << "Unknown integrator " << integratorType << endl;
			Simulation::exit(1);
		}
		_integrator->readXML(xmlconfig);
		_integrator->init();
		xmlconfig.changecurrentnode("..");
	} else {
		global_log->error() << "Integrator section missing." << endl;
	}

	/* run section */
	if(xmlconfig.changecurrentnode("run")) {
		xmlconfig.getNodeValueReduced("currenttime", _simulationTime);
		global_log->info() << "Simulation start time: " << _simulationTime << endl;
		/* steps */
		xmlconfig.getNodeValue("equilibration/steps", _initStatistics);
		global_log->info() << "Number of equilibration steps: " << _initStatistics << endl;
		xmlconfig.getNodeValue("production/steps", _numberOfTimesteps);
		global_log->info() << "Number of timesteps: " << _numberOfTimesteps << endl;
		xmlconfig.changecurrentnode("..");
	} else {
		global_log->error() << "Run section missing." << endl;
	}

	/* ensemble */
	if(xmlconfig.changecurrentnode("ensemble")) {
		string ensembletype;
		xmlconfig.getNodeValue("@type", ensembletype);
		global_log->info() << "Ensemble: " << ensembletype<< endl;
		if (ensembletype == "NVT") {
			if(_ensemble != NULL) delete _ensemble;
			_ensemble = new CanonicalEnsemble();
		} else if (ensembletype == "muVT") {
			global_log->error() << "muVT ensemble not completely implemented via XML input." << endl;
			Simulation::exit(1);
			// _ensemble = new GrandCanonicalEnsemble();
		} else {
			global_log->error() << "Unknown ensemble type: " << ensembletype << endl;
			Simulation::exit(1);
		}
		_ensemble->readXML(xmlconfig);
		/** @todo Here we store data in the _domain member as long as we do not use the ensemble everywhere */
		for (int d = 0; d < 3; d++) {
			_domain->setGlobalLength(d, _ensemble->domain()->length(d));
		}
		_domain->setGlobalTemperature(_ensemble->T());
		xmlconfig.changecurrentnode("..");
	}
	else {
		global_log->error() << "Ensemble section missing." << endl;
		Simulation::exit(1);
	}

	/* NEMD */
	{
		long numFeaturesNEMD = 0;
		XMLfile::Query query = xmlconfig.query("NEMD/feature");
		numFeaturesNEMD = query.card();
		global_log->info() << "Number of NEMD features: " << numFeaturesNEMD << endl;
		if(numFeaturesNEMD < 1) {
			global_log->info() << "No NEMD features specified." << endl;
		}

		string oldpath = xmlconfig.getcurrentnodepath();
		XMLfile::Query::const_iterator featureNEMDIter;
		for( featureNEMDIter = query.begin(); featureNEMDIter; featureNEMDIter++ ) {
			xmlconfig.changecurrentnode( featureNEMDIter );
			std::string featureName("unknown");
			xmlconfig.getNodeValue("@name", featureName);
			global_log->info() << "Enabling NEMD feature: " << featureName << endl;
			if(featureName == "DistControl") {
				_distControl = new DistControl(_domainDecomposition, _domain);
				_distControl->readXML(xmlconfig);
				_distControl->Init(_moleculeContainer);
			}
			else if(featureName == "RegionSampling") {
				_regionSampling = new RegionSampling(_domain, _domainDecomposition);
				_regionSampling->readXML(xmlconfig);
			}
			else {
				global_log->error() << "Unknown NEMD feature: " <<  featureName << "! Program exit..." << endl;
				Simulation::exit(-1);
			}
		}
		xmlconfig.changecurrentnode(oldpath);
	}

	/* algorithm */
	if(xmlconfig.changecurrentnode("algorithm")) {
		/* cutoffs */
		if(xmlconfig.changecurrentnode("cutoffs")) {
			if(xmlconfig.getNodeValueReduced("defaultCutoff", _cutoffRadius)) {
				global_log->info() << "dimensionless default cutoff radius:\t" << _cutoffRadius << endl;
			}
			if(xmlconfig.getNodeValueReduced("radiusLJ", _LJCutoffRadius)) {
				global_log->info() << "dimensionless LJ cutoff radius:\t" << _LJCutoffRadius << endl;
			}
			/** @todo introduce maxCutoffRadius here for datastructures, ...
			 *        maybe use map/list to store cutoffs for different potentials? */
			_cutoffRadius = max(_cutoffRadius, _LJCutoffRadius);
			if(_cutoffRadius <= 0) {
				global_log->error() << "cutoff radius <= 0." << endl;
				Simulation::exit(1);
			}
			global_log->info() << "dimensionless cutoff radius:\t" << _cutoffRadius << endl;
			xmlconfig.changecurrentnode("..");
		} else {
			global_log->error() << "Cutoff section missing." << endl;
			Simulation::exit(1);
		}

		/* electrostatics */
		/** @todo This may be better go into a physical section for constants? */
		if(xmlconfig.changecurrentnode("electrostatic[@type='ReactionField']")) {
			double epsilonRF = 0;
			xmlconfig.getNodeValueReduced("epsilon", epsilonRF);
			global_log->info() << "Epsilon Reaction Field: " << epsilonRF << endl;
			_domain->setepsilonRF(epsilonRF);
			xmlconfig.changecurrentnode("..");
		} else {
			global_log->error() << "Electrostatics section for reaction field setup missing." << endl;
			Simulation::exit(1);
		}

		if (xmlconfig.changecurrentnode("electrostatic[@type='FastMultipoleMethod']")) {
			_FMM = new bhfmm::FastMultipoleMethod();
			_FMM->readXML(xmlconfig);
			xmlconfig.changecurrentnode("..");
		}

		/* parallelisation */
		if(xmlconfig.changecurrentnode("parallelisation")) {
			string parallelisationtype("DomainDecomposition");
			xmlconfig.getNodeValue("@type", parallelisationtype);
			global_log->info() << "Parallelisation type: " << parallelisationtype << endl;
		#ifdef ENABLE_MPI
			if(parallelisationtype == "DummyDecomposition") {
				global_log->error() << "DummyDecomposition not available in parallel mode." << endl;
				//_domainDecomposition = new DomainDecompDummy();
			}
			else if(parallelisationtype == "DomainDecomposition") {
				if (_domainDecomposition != nullptr) {
					delete _domainDecomposition;
				}
				_domainDecomposition = new DomainDecomposition();
			}
			else if(parallelisationtype == "KDDecomposition") {
				if (_domainDecomposition != nullptr) {
					delete _domainDecomposition;
				}
				_domainDecomposition = new KDDecomposition(getcutoffRadius(), _domain);
			}
			else {
				global_log->error() << "Unknown parallelisation type: " << parallelisationtype << endl;
				Simulation::exit(1);
			}
		#else /* serial */
			if(parallelisationtype != "DummyDecomposition") {
				global_log->warning()
						<< "Executable was compiled without support for parallel execution: "
						<< parallelisationtype
						<< " not available. Using serial mode." << endl;
				//Simulation::exit(1);
			}
			//_domainDecomposition = new DomainDecompBase();  // already set in initialize()
		#endif
			_domainDecomposition->readXML(xmlconfig);
			xmlconfig.changecurrentnode("..");
		}
		else {
		#ifdef ENABLE_MPI
			global_log->error() << "Parallelisation section missing." << endl;
			Simulation::exit(1);
		#else /* serial */
			//_domainDecomposition = new DomainDecompBase(); // already set in initialize()
		#endif
		}

		/* datastructure */
		if(xmlconfig.changecurrentnode("datastructure")) {
			string datastructuretype;
			xmlconfig.getNodeValue("@type", datastructuretype);
			global_log->info() << "Datastructure type: " << datastructuretype << endl;
			if(datastructuretype == "LinkedCells") {
				_moleculeContainer = new LinkedCells();
				/** @todo Review if we need to know the max cutoff radius usable with any datastructure. */
				global_log->info() << "Setting cell cutoff radius for linked cell datastructure to " << _cutoffRadius << endl;
				LinkedCells *lc = static_cast<LinkedCells*>(_moleculeContainer);
				lc->setCutoff(_cutoffRadius);
			}
			else if(datastructuretype == "AdaptiveSubCells") {
				global_log->warning() << "AdaptiveSubCells no longer supported." << std::endl;
				Simulation::exit(-1);
			}
			else {
				global_log->error() << "Unknown data structure type: " << datastructuretype << endl;
				Simulation::exit(1);
			}
			_moleculeContainer->readXML(xmlconfig);

			double bBoxMin[3];
			double bBoxMax[3];
			_domainDecomposition->getBoundingBoxMinMax(_domain, bBoxMin, bBoxMax);
			_moleculeContainer->rebuild(bBoxMin, bBoxMax);
			xmlconfig.changecurrentnode("..");
		} else {
			global_log->error() << "Datastructure section missing" << endl;
			Simulation::exit(1);
		}

		/* thermostats */
		if(xmlconfig.changecurrentnode("thermostats")) {
			long numThermostats = 0;
			XMLfile::Query query = xmlconfig.query("thermostat");
			numThermostats = query.card();
			global_log->info() << "Number of thermostats: " << numThermostats << endl;
			if(numThermostats > 1) {
				global_log->info() << "Enabling component wise thermostat" << endl;
				_velocityScalingThermostat.enableComponentwise();
			}
			string oldpath = xmlconfig.getcurrentnodepath();
			XMLfile::Query::const_iterator thermostatIter;
			for( thermostatIter = query.begin(); thermostatIter; thermostatIter++ ) {
				xmlconfig.changecurrentnode( thermostatIter );
				string thermostattype;
				xmlconfig.getNodeValue("@type", thermostattype);
				if(thermostattype == "VelocityScaling") {
					double temperature = _ensemble->T();
					xmlconfig.getNodeValue("temperature", temperature);
					string componentName("global");
					xmlconfig.getNodeValue("@componentId", componentName);
					if(componentName == "global"){
						_domain->setGlobalTemperature(temperature);
						global_log->info() << "Adding global velocity scaling thermostat, T = " << temperature << endl;
					}
					else {
						int componentId = 0;
						componentId = getEnsemble()->getComponent(componentName)->ID();
						int thermostatID = _domain->getThermostat(componentId);
						_domain->setTargetTemperature(thermostatID, temperature);
						global_log->info() << "Adding velocity scaling thermostat for component '" << componentName << "' (ID: " << componentId << "), T = " << temperature << endl;
					}
				}
				else if(thermostattype == "TemperatureControl")
				{
					if(NULL == _temperatureControl)
					{
						_temperatureControl = new TemperatureControl(_domain, _domainDecomposition);
						_temperatureControl->readXML(xmlconfig);
					}
					else
					{
						global_log->error() << "Instance of TemperatureControl allready exist! Programm exit ..." << endl;
						Simulation::exit(-1);
					}
				}
				else
				{
					global_log->warning() << "Unknown thermostat " << thermostattype << endl;
					continue;
				}
			}
			xmlconfig.changecurrentnode(oldpath);
			xmlconfig.changecurrentnode("..");
		}
		else {
			global_log->warning() << "Thermostats section missing." << endl;
		}
		
		/* long range correction */
		if(xmlconfig.changecurrentnode("longrange") )
		{
			std::string type;
			if( !xmlconfig.getNodeValue("@type", type) )
			{
				global_log->error() << "LongRangeCorrection: Missing type specification. Program exit ..." << endl;
				Simulation::exit(-1);
			}
			if("planar" == type)
			{
				unsigned int nSlabs = 10;
				_longRangeCorrection = new Planar(_cutoffRadius, _LJCutoffRadius, _domain, _domainDecomposition, _moleculeContainer, nSlabs, global_simulation);
				_longRangeCorrection->readXML(xmlconfig);
			}
			else if("homogeneous" == type)
			{
				/*
				 * Needs to be initialized later for some reason, done in Simulation::prepare_start()
				 * TODO: perhabs work on this, to make it more robust 
				 *
				 *_longRangeCorrection = new Homogeneous(_cutoffRadius, _LJCutoffRadius,_domain,global_simulation);
                 */
			}
			else
			{
				global_log->error() << "LongRangeCorrection: Wrong type. Expected type == homogeneous|planar. Program exit ..." << endl;
                Simulation::exit(-1);
			}
			xmlconfig.changecurrentnode("..");
		}

		xmlconfig.changecurrentnode(".."); /* algorithm section */
	}
	else {
		global_log->error() << "Algorithm section missing." << endl;
	}

	/* output */
	long numOutputPlugins = 0;
	XMLfile::Query query = xmlconfig.query("output/outputplugin");
	numOutputPlugins = query.card();
	global_log->info() << "Number of output plugins: " << numOutputPlugins << endl;
	if(numOutputPlugins < 1) {
		global_log->warning() << "No output plugins specified." << endl;
	}

	string oldpath = xmlconfig.getcurrentnodepath();
	XMLfile::Query::const_iterator outputPluginIter;
	for( outputPluginIter = query.begin(); outputPluginIter; outputPluginIter++ ) {
		xmlconfig.changecurrentnode( outputPluginIter );
		OutputBase *outputPlugin = NULL;
		string pluginname("");
		xmlconfig.getNodeValue("@name", pluginname);
		global_log->info() << "Enabling output plugin: " << pluginname << endl;
		if(pluginname == "CheckpointWriter") {
			std::string strType = "unknown";
			xmlconfig.getNodeValue("@type", strType);

			if("ASCII" == strType)
				outputPlugin = new CheckpointWriter();
			else if("binary" == strType)
				outputPlugin = new BinaryCheckpointWriter();
			else
			{
				global_log->error() << "Unknown CheckpointWriter type, expected: ASCII|binary. Program exit... " << endl;
				Simulation::exit(-1);
			}
		}
		else if(pluginname == "DecompWriter") {
			outputPlugin = new DecompWriter();
		}
		else if(pluginname == "FLOPCounter") {
			/** @todo  Make the Flop counter a real output plugin */
			_flopCounter = new FlopCounter(_cutoffRadius, _LJCutoffRadius);
			continue;
		}
		else if(pluginname == "MmspdWriter") {
			outputPlugin = new MmspdWriter();
		}
		else if(pluginname == "MmspdBinWriter") {
			outputPlugin = new MmspdBinWriter();
		}
		else if(pluginname == "MmpldWriter") {
			std::string strType = "unknown";
			xmlconfig.getNodeValue("@type", strType);

			if("simple" == strType)
				outputPlugin = new MmpldWriterSimpleSphere();
			else if("multi" == strType)
				outputPlugin = new MmpldWriterMultiSphere ();
			else
			{
				global_log->error() << "MmpldWriter: wrong attribute, expected type=simple|multi. Program exit... " << endl;
				Simulation::exit(-1);
			}
		}
		else if(pluginname == "PovWriter") {
			outputPlugin = new PovWriter();
		}
		else if(pluginname == "RDF") {
			_rdf = new RDF();
			outputPlugin = _rdf;
		}
		else if(pluginname == "Resultwriter") {
			outputPlugin = new ResultWriter();
		}
		else if(pluginname == "SysMonOutput") {
			outputPlugin = new SysMonOutput();
		}
		else if(pluginname == "VISWriter") {
			outputPlugin = new VISWriter();
		}
#ifdef VTK
		else if(pluginname == "VTKMoleculeWriter") {
			outputPlugin = new VTKMoleculeWriter();
		}
		else if(pluginname == "VTKGridWriter") {
			outputPlugin = new VTKGridWriter();
		}
#endif /* VTK */
		else if(pluginname == "XyzWriter") {
			outputPlugin = new XyzWriter();
		}
		else if(pluginname == "CavityWriter") {
			outputPlugin = new CavityWriter();
		}
        else if(pluginname == "GammaWriter") {
            outputPlugin = new GammaWriter();
        }
		/* temporary */
		else if(pluginname == "MPICheckpointWriter") {
			outputPlugin = new MPICheckpointWriter();
		}
		else if(pluginname == "VectorizationTuner") {
			outputPlugin = new VectorizationTuner(_cutoffRadius, _LJCutoffRadius, &_cellProcessor);
		}
		else if(pluginname == "FlopRateWriter") {
			outputPlugin = new FlopRateWriter(_cutoffRadius, _LJCutoffRadius);
		}
		else {
			global_log->warning() << "Unknown plugin " << pluginname << endl;
			continue;
		}

		if(NULL != outputPlugin)
		{
			outputPlugin->readXML(xmlconfig);
			_outputPlugins.push_back(outputPlugin);
		}
	}
	xmlconfig.changecurrentnode(oldpath);
}

void Simulation::readConfigFile(string filename) {
	string extension(getFileExtension(filename.c_str()));
	global_log->debug() << "Found config filename extension: " << extension << endl;
	if (extension == "xml") {
		initConfigXML(filename);
	}
	else if (extension == "cfg") {
		initConfigOldstyle(filename);
	}
	else {
		global_log->error() << "Unknown config file extension '" << extension << "'." << endl;
		Simulation::exit(1);;
	}
}

void Simulation::initConfigXML(const string& inputfilename) {
	global_log->info() << "Initializing XML config file: " << inputfilename << endl;
	XMLfileUnits inp(inputfilename);

	global_log->debug() << "Input XML:" << endl << string(inp) << endl;

	if(inp.changecurrentnode("/mardyn") < 0) {
		global_log->error() << "Cound not find root node /mardyn." << endl;
		global_log->error() << "Not a valid MarDyn XML input file." << endl;
		Simulation::exit(1);
	}

	string version("unknown");
	inp.getNodeValue("@version", version);
	global_log->info() << "MarDyn XML config file version: " << version << endl;

	if (inp.changecurrentnode("simulation")) {
		/** @todo this is all for old input files. Remove! */
		string siminpfile;
		int numsimpfiles = inp.getNodeValue("input", siminpfile);
		if (numsimpfiles == 1) {
			string siminptype;
			global_log->info() << "Reading input file: " << siminpfile << endl;
			inp.getNodeValue("input@type", siminptype);
			global_log->info() << "Input file type: " << siminptype << endl;
			if (siminptype == "oldstyle") {
				initConfigOldstyle(siminpfile);
				/* Skip the rest of the xml config for old cfg files. */
				return;
			} else {
				global_log->error() << "Unknown input file type: " << siminptype << endl;
				Simulation::exit(1);;
			}
		} else if (numsimpfiles > 1) {
			global_log->error() << "Multiple input file sections are not supported." << endl;
			Simulation::exit(1);
		}

		readXML(inp);

		string pspfile;
		string pspfileheader;
		string pspfiletype("ASCII");
		if (inp.getNodeValue("ensemble/phasespacepoint/file@type", pspfiletype) ){
			if (pspfiletype == "ASCII") {
				if (inp.getNodeValue("ensemble/phasespacepoint/file", pspfile)) {
					pspfile.insert(0, inp.getDir());
					global_log->info() << "phasespacepoint description file:\t"
							<< pspfile << endl;
				}
				_inputReader = (InputBase*) new InputOldstyle();
				_inputReader->setPhaseSpaceFile(pspfile);
			}
			else if (pspfiletype == "binary") {
				if (inp.getNodeValue("ensemble/phasespacepoint/file/header", pspfileheader)) {
					pspfileheader.insert(0, inp.getDir());
					global_log->info() << "phasespacepoint description file:\t"
							<< pspfileheader << endl;
				}
				if (inp.getNodeValue("ensemble/phasespacepoint/file/data", pspfile)) {
					pspfile.insert(0, inp.getDir());
				}
				_inputReader = (InputBase*) new BinaryReader();
				_inputReader->setPhaseSpaceHeaderFile(pspfileheader);
				_inputReader->setPhaseSpaceFile(pspfile);

				// read header
				double timestepLength = 0.005;  // <-- TODO: should be removed from parameter list
				_inputReader->readPhaseSpaceHeader(_domain, timestepLength);
			}
			else {
				global_log->error() << "Unknown type in node: ensemble/phasespacepoint/file, "
						"expected: ASCII|binary. Programm exit ..." << endl;
				Simulation::exit(-1);
			}
			global_log->info() << "       phasespacepoint file type:\t"
					<< pspfiletype << endl;
		}
		string oldpath = inp.getcurrentnodepath();
		if(inp.changecurrentnode("ensemble/phasespacepoint/generator")) {
			string generatorName;
			inp.getNodeValue("@name", generatorName);
			global_log->info() << "Generator: " << generatorName << endl;
			if(generatorName == "GridGenerator") {
				_inputReader = new GridGenerator();
			}
			else if(generatorName == "mkesfera") {
				_inputReader = new MkesferaGenerator();
			}
			else if(generatorName == "mkTcTS") {
				_inputReader = new MkTcTSGenerator();
			}
			else {
				global_log->error() << "Unknown generator: " << generatorName << endl;
				Simulation::exit(1);
			}
			_inputReader->readXML(inp);
		}
		inp.changecurrentnode(oldpath);


		inp.changecurrentnode("..");
	} // simulation-section
	else {
		global_log->error() << "Simulation section missing" << endl;
		Simulation::exit(1);
	}

#ifdef ENABLE_MPI
	// if we are using the DomainDecomposition, please complete its initialization:
	{
		DomainDecomposition * temp = nullptr;
		temp = dynamic_cast<DomainDecomposition *>(_domainDecomposition);
		if (temp != nullptr) {
			temp->initCommunicationPartners(_cutoffRadius, _domain);
		}
	}
#endif

	// read particle data (or generate particles, if a generator is chosen)
	unsigned long maxid = _inputReader->readPhaseSpace(_moleculeContainer,
			&_lmu, _domain, _domainDecomposition);


	_domain->initParameterStreams(_cutoffRadius, _LJCutoffRadius);
	//domain->initFarFieldCorr(_cutoffRadius, _LJCutoffRadius);

	// test new Decomposition
	_moleculeContainer->update();
	_moleculeContainer->deleteOuterParticles();

	int ownrank = 0;
#ifdef ENABLE_MPI
	MPI_CHECK( MPI_Comm_rank(MPI_COMM_WORLD, &ownrank) );
#endif
	unsigned idi = _lmu.size();
	unsigned j = 0;
	std::list<ChemicalPotential>::iterator cpit;
	for (cpit = _lmu.begin(); cpit != _lmu.end(); cpit++) {
		cpit->setIncrement(idi);
		double tmp_molecularMass = global_simulation->getEnsemble()->getComponent(cpit->getComponentID())->m();
		cpit->setSystem(_domain->getGlobalLength(0),
				_domain->getGlobalLength(1), _domain->getGlobalLength(2),
				tmp_molecularMass);
		cpit->setGlobalN(global_simulation->getEnsemble()->getComponent(cpit->getComponentID())->getNumMolecules());
		cpit->setNextID(j + (int) (1.001 * (256 + maxid)));

		cpit->setSubdomain(ownrank, _moleculeContainer->getBoundingBoxMin(0),
				_moleculeContainer->getBoundingBoxMax(0),
				_moleculeContainer->getBoundingBoxMin(1),
				_moleculeContainer->getBoundingBoxMax(1),
				_moleculeContainer->getBoundingBoxMin(2),
				_moleculeContainer->getBoundingBoxMax(2));
		/* TODO: thermostat */
		double Tcur = _domain->getCurrentTemperature(0);
		/* FIXME: target temperature from thermostat ID 0 or 1?  */
		double
				Ttar =
						_domain->severalThermostats() ? _domain->getTargetTemperature(
								1)
								: _domain->getTargetTemperature(0);
		if ((Tcur < 0.85 * Ttar) || (Tcur > 1.15 * Ttar))
			Tcur = Ttar;
		cpit->submitTemperature(Tcur);
		if (h != 0.0)
			cpit->setPlanckConstant(h);

		j++;
	}
}

void Simulation::prepare_start() {
	global_log->info() << "Initializing simulation" << endl;

	global_log->info() << "Initialising cell processor" << endl;
#if ENABLE_VECTORIZED_CODE
	global_log->debug() << "Checking if vectorized cell processor can be used" << endl;
	bool lj_present = false;
	bool charge_present = false;
	bool dipole_present = false;
	bool quadrupole_present = false;

	const vector<Component> components = *(global_simulation->getEnsemble()->getComponents());
	for (size_t i = 0; i < components.size(); i++) {
		lj_present |= (components[i].numLJcenters() != 0);
		charge_present |= (components[i].numCharges() != 0);
		dipole_present |= (components[i].numDipoles() != 0);
		quadrupole_present |= (components[i].numQuadrupoles() != 0);
	}
	global_log->debug() << "xx lj present: " << lj_present << endl;
	global_log->debug() << "xx charge present: " << charge_present << endl;
	global_log->debug() << "xx dipole present: " << dipole_present << endl;
	global_log->debug() << "xx quadrupole present: " << quadrupole_present << endl;

	/*if(this->_lmu.size() > 0) {
		global_log->warning() << "Using legacy cell processor. (The vectorized code does not support grand canonical simulations.)" << endl;
		_cellProcessor = new LegacyCellProcessor( _cutoffRadius, _LJCutoffRadius, _particlePairsHandler);
	}
	else*/
#ifndef MARDYN_WR
	if(this->_doRecordVirialProfile) {
		global_log->warning() << "Using legacy cell processor. (The vectorized code does not support the virial tensor and the localized virial profile.)" << endl;
		_cellProcessor = new LegacyCellProcessor(_cutoffRadius, _LJCutoffRadius, _particlePairsHandler);
	} else if (_rdf != NULL) {
		global_log->warning() << "Using legacy cell processor. (The vectorized code does not support rdf sampling.)"
				<< endl;
		_cellProcessor = new LegacyCellProcessor(_cutoffRadius, _LJCutoffRadius, _particlePairsHandler);
	} else {
		global_log->info() << "Using vectorized cell processor." << endl;
		_cellProcessor = new VectorizedCellProcessor( *_domain, _cutoffRadius, _LJCutoffRadius);
	}
#else
	global_log->info() << "Using WR cell processor." << endl;
	_cellProcessor = new VCP1CLJ_WR( *_domain, _cutoffRadius, _LJCutoffRadius);
#endif /* MARDYN_WR */
#else
	global_log->info() << "Using legacy cell processor." << endl;
	_cellProcessor = new LegacyCellProcessor( _cutoffRadius, _LJCutoffRadius, _particlePairsHandler);
#endif

	if (_FMM != NULL) {

		double globalLength[3];
		for (int i = 0; i < 3; i++) {
			globalLength[i] = _domain->getGlobalLength(i);
		}
		double bBoxMin[3];
		double bBoxMax[3];
		for (int i = 0; i < 3; i++) {
			bBoxMin[i] = _domainDecomposition->getBoundingBoxMin(i, _domain);
			bBoxMax[i] = _domainDecomposition->getBoundingBoxMax(i, _domain);
		}
		_FMM->init(globalLength, bBoxMin, bBoxMax,
				dynamic_cast<LinkedCells*>(_moleculeContainer)->cellLength(), _moleculeContainer);

		delete _cellProcessor;
		_cellProcessor = new bhfmm::VectorizedLJP2PCellProcessor(*_domain, _LJCutoffRadius, _cutoffRadius);
	}

	global_log->info() << "Clearing halos" << endl;
	_moleculeContainer->deleteOuterParticles();
	global_log->info() << "Updating domain decomposition" << endl;

	_memoryProfiler->doOutput("without halo copies");

	// temporary addition until MPI communication is parallelized with OpenMP
	//we don't actually need the mpiOMPCommunicationTimer here -> deactivate it..
	global_simulation->deactivateTimer("SIMULATION_MPI_OMP_COMMUNICATION");
	updateParticleContainerAndDecomposition();
	global_simulation->activateTimer("SIMULATION_MPI_OMP_COMMUNICATION");

	_memoryProfiler->doOutput("with halo copies");

#ifndef MARDYN_WR
	global_log->info() << "Performing initial force calculation" << endl;
	global_simulation->startTimer("SIMULATION_FORCE_CALCULATION");
	_moleculeContainer->traverseCells(*_cellProcessor);
	global_simulation->stopTimer("SIMULATION_FORCE_CALCULATION");

	_loopCompTime = global_simulation->getTime("SIMULATION_FORCE_CALCULATION");
	global_simulation->resetTimer("SIMULATION_FORCE_CALCULATION");
	++_loopCompTimeSteps;
#else
	global_log->info() << "No initial force calculation needed in WR mode" << endl;
#endif /* MARDYN_WR */

	if (_FMM != NULL) {
		global_log->info() << "Performing initial FMM force calculation" << endl;
		_FMM->computeElectrostatics(_moleculeContainer);
	}

	/* If enabled count FLOP rate of LS1. */
	if( NULL != _flopCounter ) {
		_moleculeContainer->traverseCells(*_flopCounter);
	}

    // clear halo
    global_log->info() << "Clearing halos" << endl;
    _moleculeContainer->deleteOuterParticles();

    if (_longRangeCorrection == NULL){
        _longRangeCorrection = new Homogeneous(_cutoffRadius, _LJCutoffRadius,_domain,global_simulation);
    }


    _longRangeCorrection->calculateLongRange();

	// here we have to call calcFM() manually, otherwise force and moment are not
	// updated inside the molecule (actually this is done in upd_postF)
	// integrator->eventForcesCalculated should not be called, since otherwise the velocities would already be updated.
	#if defined(_OPENMP)
	#pragma omp parallel
	#endif
	{
		const ParticleIterator begin = _moleculeContainer->iteratorBegin();
		const ParticleIterator end = _moleculeContainer->iteratorEnd();

		for (ParticleIterator i = begin; i != end; ++i){
			i->calcFM();
		}
	} // end pragma omp parallel

	if (_pressureGradient->isAcceleratingUniformly()) {
		global_log->info() << "Initialising uniform acceleration." << endl;
		unsigned long uCAT = _pressureGradient->getUCAT();
		global_log->info() << "uCAT: " << uCAT << " steps." << endl;
		_pressureGradient->determineAdditionalAcceleration(
				_domainDecomposition, _moleculeContainer, uCAT
						* _integrator->getTimestepLength());
		global_log->info() << "Uniform acceleration initialised." << endl;
	}

	global_log->info() << "Calculating global values" << endl;
	_domain->calculateThermostatDirectedVelocity(_moleculeContainer);

	_domain->calculateVelocitySums(_moleculeContainer);

	_domain->calculateGlobalValues(_domainDecomposition, _moleculeContainer,
			true, 1.0);
	global_log->debug() << "Calculating global values finished." << endl;

	if (_lmu.size() + _mcav.size() > 0) {
		/* TODO: thermostat */
		double Tcur = _domain->getGlobalCurrentTemperature();
		/* FIXME: target temperature from thermostat ID 0 or 1? */
		double
				Ttar = _domain->severalThermostats() ? _domain->getTargetTemperature(1)
								: _domain->getTargetTemperature(0);
		if ((Tcur < 0.85 * Ttar) || (Tcur > 1.15 * Ttar))
			Tcur = Ttar;

		list<ChemicalPotential>::iterator cpit;
		if (h == 0.0)
			h = sqrt(6.2831853 * Ttar);
		for (cpit = _lmu.begin(); cpit != _lmu.end(); cpit++) {
			cpit->submitTemperature(Tcur);
			cpit->setPlanckConstant(h);
		}
                map<unsigned, CavityEnsemble>::iterator ceit;
		for (ceit = _mcav.begin(); ceit != _mcav.end(); ceit++) {
		   ceit->second.submitTemperature(Tcur);
		}
	}

	// initial number of timesteps
	_initSimulation = (unsigned long) (this->_simulationTime / _integrator->getTimestepLength());

	// initialize output
	std::list<OutputBase*>::iterator outputIter;
	for (outputIter = _outputPlugins.begin(); outputIter
			!= _outputPlugins.end(); outputIter++) {
		(*outputIter)->initOutput(_moleculeContainer, _domainDecomposition,
				_domain);
	}

	global_log->info() << "System initialised\n" << endl;
	global_log->info() << "System contains "
			<< _domain->getglobalNumMolecules() << " molecules." << endl;

    // Init NEMD feature objects
	if(NULL != _distControl)
	{
		_distControl->SetDomainDecomposition(_domainDecomposition);
		_distControl->UpdatePositionsInit(_moleculeContainer);
		_distControl->WriteHeader();
		_distControl->WriteData(0);
	}

    // Init control instances (data structures)
    if(NULL != _regionSampling)
    {
    	_regionSampling->SetDomainDecomposition(_domainDecomposition);
    	_regionSampling->PrepareRegionSubdivisions();
        _regionSampling->Init();
    }

    if(NULL != _temperatureControl)
    {
		_temperatureControl->PrepareRegionSubdivisions();
    	_temperatureControl->PrepareRegionDataStructures();
    }

    if(NULL != _densityControl)
    {
    	_densityControl->Init(_densityControl->GetControlFreq() );
    	_densityControl->CheckRegionBounds();
		_densityControl->SetFlagsNEMD(_flagsNEMD);
    }

	// PARTICLE_TRACKER
	if(NULL != _particleTracker)
	{
		_particleTracker->Prepare();
	}
}

void Simulation::simulate() {
	global_log->info() << "Started simulation" << endl;

	// (universal) constant acceleration (number of) timesteps
	unsigned uCAT = _pressureGradient->getUCAT();
// 	_initSimulation = (unsigned long) (_domain->getCurrentTime()
// 			/ _integrator->getTimestepLength());
//    _initSimulation = (unsigned long) (this->_simulationTime / _integrator->getTimestepLength());
	// _initSimulation = 1;
	/* demonstration for the usage of the new ensemble class */
	/*CanonicalEnsemble ensemble(_moleculeContainer, global_simulation->getEnsemble()->getComponents());
	ensemble.updateGlobalVariable(NUM_PARTICLES);
	global_log->debug() << "Number of particles in the Ensemble: "
			<< ensemble.N() << endl;
	ensemble.updateGlobalVariable(ENERGY);
	global_log->debug() << "Kinetic energy in the Ensemble: " << ensemble.E()
		<< endl;
	ensemble.updateGlobalVariable(TEMPERATURE);
	global_log->debug() << "Temperature of the Ensemble: " << ensemble.T()
		<< endl;*/

	/***************************************************************************/
	/* BEGIN MAIN LOOP                                                         */
	/***************************************************************************/

	// all timers except the ioTimer measure inside the main loop
	Timer* loopTimer = global_simulation->getTimer("SIMULATION_LOOP"); // timer for the entire simulation loop (synced)
	global_simulation->setOutputString("SIMULATION_LOOP", "Computation in main loop took:");
	Timer* decompositionTimer = global_simulation->getTimer("SIMULATION_DECOMPOSITION"); // timer for decomposition: sub-timer of loopTimer
	global_simulation->setOutputString("SIMULATION_DECOMPOSITION", "Decomposition took:");
	Timer* computationTimer = global_simulation->getTimer("SIMULATION_COMPUTATION"); // timer for computation: sub-timer of loopTimer
	global_simulation->setOutputString("SIMULATION_COMPUTATION", "Computation took:");
	Timer* perStepIoTimer = global_simulation->getTimer("SIMULATION_PER_STEP_IO"); // timer for io in simulation loop: sub-timer of loopTimer
	global_simulation->setOutputString("SIMULATION_PER_STEP_IO", "IO in main loop took:");
	Timer* ioTimer = global_simulation->getTimer("SIMULATION_IO"); // timer for final io
	global_simulation->setOutputString("SIMULATION_IO", "Final IO took:");
	Timer* forceCalculationTimer = global_simulation->getTimer("SIMULATION_FORCE_CALCULATION"); // timer for force calculation: sub-timer of computationTimer
	global_simulation->setOutputString("SIMULATION_FORCE_CALCULATION", "Force calculation took:");
	Timer* mpiOMPCommunicationTimer = global_simulation->getTimer("SIMULATION_MPI_OMP_COMMUNICATION"); // timer for measuring MPI-OMP communication time: sub-timer of decompositionTimer
	global_simulation->setOutputString("SIMULATION_MPI_OMP_COMMUNICATION", "Communication took:");
	global_simulation->setOutputString("COMMUNICATION_PARTNER_INIT_SEND", "initSend() took:");
	global_simulation->setOutputString("COMMUNICATION_PARTNER_TEST_RECV", "testRecv() took:");

	//loopTimer->set_sync(true);
	global_simulation->setSyncTimer("SIMULATION_LOOP", true);
#if WITH_PAPI
	const char *papi_event_list[] = {
		"PAPI_TOT_CYC",
		"PAPI_TOT_INS"
	//	"PAPI_VEC_DP"
	// 	"PAPI_L2_DCM"
	// 	"PAPI_L2_ICM"
	// 	"PAPI_L1_ICM"
	//	"PAPI_DP_OPS"
	// 	"PAPI_VEC_INS"
	};
	int num_papi_events = sizeof(papi_event_list) / sizeof(papi_event_list[0]);
	loopTimer->add_papi_counters(num_papi_events, (char**) papi_event_list);
#endif
	loopTimer->start();
#ifndef NDEBUG
#ifndef ENABLE_MPI
		unsigned particleNoTest;
#endif
#endif
	_memoryProfiler->doOutput();

	for (_simstep = _initSimulation + 1; _simstep <= _numberOfTimesteps; _simstep++) {
		// Too many particle exchanges in the first 10 simulation steps.
		// Reset the timers after 10 simulation steps and restart the timers
		// 		for more accurate measurements for benchmarking.
		if (_simstep == 10) {
			loopTimer->stop();

			global_log->info() << "Simstep 10:" << endl;
			global_simulation->printTimers();
			global_log->info() << endl;
			global_log->info() << "RESETTING TIMERS" << endl;
			global_simulation->resetTimers();
			global_log->info() << endl;

			loopTimer->start();
		}

		global_log->debug() << "timestep: " << getSimulationStep() << endl;
		global_log->debug() << "simulation time: " << getSimulationTime() << endl;
		global_simulation->incrementTimerTimestepCounter();

		computationTimer->start();

		/** @todo What is this good for? Where come the numbers from? Needs documentation */
		if (_simstep >= _initGrandCanonical) {
			unsigned j = 0;
			list<ChemicalPotential>::iterator cpit;
			for (cpit = _lmu.begin(); cpit != _lmu.end(); cpit++) {
				if (!((_simstep + 2 * j + 3) % cpit->getInterval())) {
					cpit->prepareTimestep(_moleculeContainer, _domainDecomposition);
				}
				j++;
			}
		}
		if (_simstep >= _initStatistics) {
		   map<unsigned, CavityEnsemble>::iterator ceit;
		   for(ceit = this->_mcav.begin(); ceit != this->_mcav.end(); ceit++) {
			  if (!((_simstep + 2 * ceit->first + 3) % ceit->second.getInterval())) {
				 ceit->second.preprocessStep();
			  }
		   }
		}

		_integrator->eventNewTimestep(_moleculeContainer, _domain);

	    // mheinen 2015-05-29 --> DENSITY_CONTROL
	    // should done after calling eventNewTimestep() / before force calculation, because force _F[] on molecule is deleted by method Molecule::setupCache()
	    // and this method is called when component of molecule changes by calling Molecule::setComponent()
	    // halo must not be populated, because density may be calculated wrong then.

	//        int nRank = _domainDecomposition->getRank();

	//        cout << "[" << nRank << "]: " << "ProcessIsRelevant() = " << _densityControl->ProcessIsRelevant() << endl;

        unsigned long nNumMoleculesDeletedLocal = 0;
        unsigned long nNumMoleculesDeletedGlobal = 0;

        if( _densityControl != NULL  &&
            _densityControl->GetStart() < _simstep && _densityControl->GetStop() >= _simstep &&  // respect start/stop
            _simstep % _densityControl->GetControlFreq() == 0 )  // respect control frequency
        {
        	/*
			// init MPI
        	_densityControl->InitMPI();

        	// only relevant processes should do the following
            if( !_densityControl->ProcessIsRelevant() )
            	break;
        	 */

            // init density control
            _densityControl->Init(_simstep);

    //            unsigned long nNumMoleculesLocal = 0;
    //            unsigned long nNumMoleculesGlobal = 0;

            for( ParticleIterator tM = _moleculeContainer->iteratorBegin();
                 tM != _moleculeContainer->iteratorEnd();
                 ++tM )
            {
                // measure density
                _densityControl->MeasureDensity(&(*tM), _simstep);

    //                nNumMoleculesLocal++;
            }

            // calc global values
            _densityControl->CalcGlobalValues(_simstep);


            bool bDeleteMolecule;

            for( ParticleIterator tM  = _moleculeContainer->iteratorBegin();
                 tM != _moleculeContainer->iteratorEnd();
                 ++tM )
            {
                bDeleteMolecule = false;

                // control density
                _densityControl->ControlDensity(&(*tM), this, _simstep, bDeleteMolecule);


                if(true == bDeleteMolecule)
                {
					unsigned long id = tM->id();
					double x, y, z;
					x = tM->r(0);
					y = tM->r(1);
					z = tM->r(2);
					_moleculeContainer->deleteMolecule(id, x, y, z, false);
                    nNumMoleculesDeletedLocal++;
                }
            }
            // write out deleted molecules data
            _densityControl->WriteDataDeletedMolecules(_simstep);
        }

        // update global number of particles
        _domainDecomposition->collCommInit(1);
        _domainDecomposition->collCommAppendUnsLong(nNumMoleculesDeletedLocal);
        _domainDecomposition->collCommAllreduceSum();
        nNumMoleculesDeletedGlobal = _domainDecomposition->collCommGetUnsLong();
        _domainDecomposition->collCommFinalize();

        _domain->setglobalNumMolecules(_domain->getglobalNumMolecules() - nNumMoleculesDeletedGlobal);

	    // <-- DENSITY_CONTROL

		// mheinen 2015-03-16 --> DISTANCE_CONTROL
		if(_distControl != NULL)
		{
			if(_distControl->GetFlag() == DCF_DEFAULT)
			{
				for( ParticleIterator tM  = _moleculeContainer->iteratorBegin();
					 tM != _moleculeContainer->iteratorEnd();
					 ++tM )
				{
					// sample density profile
					_distControl->SampleProfiles(&(*tM));
				}

				// determine interface midpoints and update region positions
				_distControl->UpdatePositions(_simstep);

				// write data
				_distControl->WriteData(_simstep);
				_distControl->WriteDataProfiles(_simstep);


				// align system center of mass
				for( ParticleIterator tM  = _moleculeContainer->iteratorBegin();
					 tM != _moleculeContainer->iteratorEnd();
					 ++tM )
				{
					_distControl->AlignSystemCenterOfMass(&(*tM), _simstep);
				}
			}
			else if(_distControl->GetFlag() == DCF_ALIGN_COM_ONLY)
			{
				if(0 == _simstep % _distControl->GetUpdateFreq() )
				{
					// sample COM
					for( ParticleIterator tM  = _moleculeContainer->iteratorBegin();
						 tM != _moleculeContainer->iteratorEnd();
						 ++tM )
					{
						_distControl->SampleCOM(&(*tM), _simstep);
					}
					// align COM

					// calc global values
					_distControl->CalcGlobalValuesCOM(_simstep);

					for( ParticleIterator tM  = _moleculeContainer->iteratorBegin();
						 tM != _moleculeContainer->iteratorEnd();
						 ++tM )
					{
						_distControl->AlignCOM(&(*tM), _simstep);
					}
				}
			}
		}
		// <-- DISTANCE_CONTROL


		// activate RDF sampling
		if ((_simstep >= _initStatistics) && _rdf != NULL) {
			global_log->info() << "Activating the RDF sampling" << endl;
			this->_rdf->tickRDF();
			this->_particlePairsHandler->setRDF(_rdf);
			this->_rdf->accumulateNumberOfMolecules(*(global_simulation->getEnsemble()->getComponents()));
		}

		/*! by Stefan Becker <stefan.becker@mv.uni-kl.de> 
		 *realignment tools borrowed from Martin Horsch, for the determination of the centre of mass 
		 *the halo MUST NOT be present*/
#ifndef NDEBUG 
#ifndef ENABLE_MPI
		particleNoTest = _moleculeContainer->getNumberOfParticles();
		global_log->info()<<"particles before determine shift-methods, halo not present:" << particleNoTest<< "\n";
#endif
#endif
        if(_doAlignCentre && !(_simstep % _alignmentInterval)) {
			if(_componentSpecificAlignment) {
				//! !!! the sequence of calling the two methods MUST be: FIRST determineXZShift() THEN determineYShift() !!!
				_domain->determineXZShift(_domainDecomposition, _moleculeContainer, _alignmentCorrection);
				_domain->determineYShift(_domainDecomposition, _moleculeContainer, _alignmentCorrection);
			}
			// edited by Michaela Heier --> realign can be used when LJ93-Potential will be used. Only the shift in the xz-plane will be used. 
			else if(_doAlignCentre && _applyWallFun_LJ_9_3){
				global_log->info() << "realign in the xz-plane without a shift in y-direction\n";
				_domain->determineXZShift(_domainDecomposition, _moleculeContainer, _alignmentCorrection);
				_domain->noYShift(_domainDecomposition, _moleculeContainer, _alignmentCorrection);
			}
			else if(_doAlignCentre && _applyWallFun_LJ_10_4){
				global_log->info() << "realign in the xz-plane without a shift in y-direction\n";
				_domain->determineXZShift(_domainDecomposition, _moleculeContainer, _alignmentCorrection);
				_domain->noYShift(_domainDecomposition, _moleculeContainer, _alignmentCorrection);
			}
			else{
				_domain->determineShift(_domainDecomposition, _moleculeContainer, _alignmentCorrection);
			}
#ifndef NDEBUG 
#ifndef ENABLE_MPI			
			particleNoTest = _moleculeContainer->getNumberOfParticles();
			global_log->info()<<"particles after determine shift-methods, halo not present:" << particleNoTest<< "\n";
#endif
#endif
		}
		computationTimer->stop();



#if defined(ENABLE_MPI) && defined(ENABLE_OVERLAPPING)
		bool overlapCommComp = true;
#else
		bool overlapCommComp = false;
#endif

		if (overlapCommComp) {
			performOverlappingDecompositionAndCellTraversalStep();
		}
		else {
			decompositionTimer->start();
			// ensure that all Particles are in the right cells and exchange Particles
			global_log->debug() << "Updating container and decomposition" << endl;
			updateParticleContainerAndDecomposition();
			decompositionTimer->stop();

			double startEtime = computationTimer->get_etime();
			// Force calculation and other pair interaction related computations
			global_log->debug() << "Traversing pairs" << endl;
			computationTimer->start();
			forceCalculationTimer->start();
			_moleculeContainer->traverseCells(*_cellProcessor);
			forceCalculationTimer->stop();
			computationTimer->stop();

			_loopCompTime += computationTimer->get_etime() - startEtime;
			_loopCompTimeSteps ++;
		}
		computationTimer->start();
		if (_FMM != NULL) {
			global_log->debug() << "Performing FMM calculation" << endl;
			_FMM->computeElectrostatics(_moleculeContainer);
		}



		if(_wall && _applyWallFun_LJ_9_3){
		  _wall->calcTSLJ_9_3(_moleculeContainer, _domain);
		}

		if(_wall && _applyWallFun_LJ_10_4){
		  _wall->calcTSLJ_10_4(_moleculeContainer, _domain);
		}
		
		if(_mirror && _applyMirror){
		  _mirror->VelocityChange(_moleculeContainer, _domain);
		}

		/** @todo For grand canonical ensemble? Should go into appropriate ensemble class. Needs documentation. */
		// test deletions and insertions
		if (_simstep >= _initGrandCanonical) {
			unsigned j = 0;
			list<ChemicalPotential>::iterator cpit;
			for (cpit = _lmu.begin(); cpit != _lmu.end(); cpit++) {
				if (!((_simstep + 2 * j + 3) % cpit->getInterval())) {
					global_log->debug() << "Grand canonical ensemble(" << j << "): test deletions and insertions"
							<< endl;
					this->_domain->setLambda(cpit->getLambda());
					this->_domain->setDensityCoefficient(cpit->getDensityCoefficient());
					double localUpotBackup = _domain->getLocalUpot();
					double localVirialBackup = _domain->getLocalVirial();
					cpit->grandcanonicalStep(_moleculeContainer, _domain->getGlobalCurrentTemperature(), this->_domain,
							_cellProcessor);
					_domain->setLocalUpot(localUpotBackup);
					_domain->setLocalVirial(localVirialBackup);
#ifndef NDEBUG
					/* check if random numbers inserted are the same for all processes... */
					cpit->assertSynchronization(_domainDecomposition);
#endif

					int localBalance = cpit->getLocalGrandcanonicalBalance();
					int balance = cpit->grandcanonicalBalance(_domainDecomposition);
					global_log->debug() << "   b[" << ((balance > 0) ? "+" : "") << balance << "("
							<< ((localBalance > 0) ? "+" : "") << localBalance << ")" << " / c = "
							<< cpit->getComponentID() << "]   " << endl;
					_domain->Nadd(cpit->getComponentID(), balance, localBalance);
				}

				j++;
			}
		}
		
		if(_simstep >= _initStatistics) {
			map<unsigned, CavityEnsemble>::iterator ceit;
			for(ceit = this->_mcav.begin(); ceit != this->_mcav.end(); ceit++) {

				unsigned cavityComponentID = ceit->first;
				CavityEnsemble & cavEns = ceit->second;

				if (!((_simstep + 2 * cavityComponentID + 3) % cavEns.getInterval())) {
					global_log->debug() << "Cavity ensemble for component " << cavityComponentID << ".\n";

					cavEns.cavityStep(this->_moleculeContainer);
				}

				if( (!((_simstep + 2 * cavityComponentID + 7) % cavEns.getInterval())) ||
					(!((_simstep + 2 * cavityComponentID + 3) % cavEns.getInterval())) ||
					(!((_simstep + 2 * cavityComponentID - 1) % cavEns.getInterval())) ) {

					// warning, return value is ignored!
					/*unsigned long ret = */ cavEns.communicateNumCavities(this->_domainDecomposition);
				}
			}
		}
		
		// clear halo
		global_log->debug() << "Deleting outer particles / clearing halo." << endl;
		_moleculeContainer->deleteOuterParticles();

		/** @todo For grand canonical ensemble? Sould go into appropriate ensemble class. Needs documentation. */
		if (_simstep >= _initGrandCanonical) {
			_domain->evaluateRho(_moleculeContainer->getNumberOfParticles(), _domainDecomposition);
		}

		if (!(_simstep % _collectThermostatDirectedVelocity))
			_domain->calculateThermostatDirectedVelocity(_moleculeContainer);
		if (_pressureGradient->isAcceleratingUniformly()) {
			if (!(_simstep % uCAT)) {
				global_log->debug() << "Determine the additional acceleration" << endl;
				_pressureGradient->determineAdditionalAcceleration(
						_domainDecomposition, _moleculeContainer, uCAT
						* _integrator->getTimestepLength());
			}
			global_log->debug() << "Process the uniform acceleration" << endl;
			_integrator->accelerateUniformly(_moleculeContainer, _domain);
			_pressureGradient->adjustTau(this->_integrator->getTimestepLength());
		}
		_longRangeCorrection->calculateLongRange();
		_longRangeCorrection->writeProfiles(_domainDecomposition, _domain, _simstep);
		
		/*
		 * radial distribution function
		 */
		if (_simstep >= _initStatistics) {
			if (this->_lmu.size() == 0) {
				this->_domain->record_cv();
			}
		}

		// Inform the integrator about the calculated forces
		global_log->debug() << "Inform the integrator" << endl;
		_integrator->eventForcesCalculated(_moleculeContainer, _domain);

		// PARTICLE_TRACKER
		if(_particleTracker != NULL)
		{
			_particleTracker->PreLoopAction(_simstep);

			for( ParticleIterator tM  = _moleculeContainer->iteratorBegin();
				 tM != _moleculeContainer->iteratorEnd();
				 ++tM )
			{
				_particleTracker->LoopAction(&(*tM));
			}  // loop over molecules

			_particleTracker->PostLoopAction();

		}  // PARTICLE_TRACKER

        // mheinen 2015-02-18 --> DRIFT_CONTROL
        if(_driftControl != NULL)
        {
            Molecule* tM;

            // init drift control
            _driftControl->Init(_simstep);

            for( ParticleIterator tM  = _moleculeContainer->iteratorBegin();
                 tM != _moleculeContainer->iteratorEnd();
                 ++tM )
            {
                // measure drift
                _driftControl->MeasureDrift(&(*tM), _simstep);

//                cout << "id = " << tM->id() << ", (vx,vy,vz) = " << tM->v(0) << ", " << tM->v(1) << ", " << tM->v(2) << endl;
            }

            // calc global values
            _driftControl->CalcGlobalValues(_simstep);

            // calc scale factors
            _driftControl->CalcScaleFactors(_simstep);

            for( ParticleIterator tM  = _moleculeContainer->iteratorBegin();
                 tM != _moleculeContainer->iteratorEnd();
                 ++tM )
            {
                // measure drift
                _driftControl->ControlDrift(&(*tM), _simstep);

//                cout << "id = " << tM->id() << ", (vx,vy,vz) = " << tM->v(0) << ", " << tM->v(1) << ", " << tM->v(2) << endl;
            }
        }
        // <-- DRIFT_CONTROL


        // mheinen 2015-03-18 --> REGION_SAMPLING
        if(_regionSampling != NULL)
        {
            for( ParticleIterator tM  = _moleculeContainer->iteratorBegin();
                 tM != _moleculeContainer->iteratorEnd();
                 ++tM )
            {
                // sample profiles and vdf
                _regionSampling->DoSampling(&(*tM), _domainDecomposition, _simstep);
            }

            // write data
            _regionSampling->WriteData(_domainDecomposition, _simstep, _domain);
        }
        // <-- REGION_SAMPLING


		// calculate the global macroscopic values from the local values
		global_log->debug() << "Calculate macroscopic values" << endl;
		_domain->calculateGlobalValues(_domainDecomposition, _moleculeContainer, 
				(!(_simstep % _collectThermostatDirectedVelocity)), Tfactor(_simstep));
		
		// scale velocity and angular momentum
		if ( !_domain->NVE() && _temperatureControl == NULL) {
			if (_thermostatType ==VELSCALE_THERMOSTAT) {
				global_log->debug() << "Velocity scaling" << endl;
				if (_domain->severalThermostats()) {
					_velocityScalingThermostat.enableComponentwise();
					for(unsigned int cid = 0; cid < global_simulation->getEnsemble()->getComponents()->size(); cid++) {
						int thermostatId = _domain->getThermostat(cid);
						_velocityScalingThermostat.setBetaTrans(thermostatId, _domain->getGlobalBetaTrans(thermostatId));
						_velocityScalingThermostat.setBetaRot(thermostatId, _domain->getGlobalBetaRot(thermostatId));
						global_log->debug() << "Thermostat for CID: " << cid << " thermID: " << thermostatId
								<< " B_trans: " << _velocityScalingThermostat.getBetaTrans(thermostatId)
								<< " B_rot: " << _velocityScalingThermostat.getBetaRot(thermostatId) << endl;
						double v[3];
						for(int d = 0; d < 3; d++) {
							v[d] = _domain->getThermostatDirectedVelocity(thermostatId, d);
						}
						_velocityScalingThermostat.setVelocity(thermostatId, v);
					}
				}
				else {
					_velocityScalingThermostat.setGlobalBetaTrans(_domain->getGlobalBetaTrans());
					_velocityScalingThermostat.setGlobalBetaRot(_domain->getGlobalBetaRot());
					/* TODO */
					// Undirected global thermostat not implemented!
				}
				_velocityScalingThermostat.apply(_moleculeContainer);


			}
			else if(_thermostatType == ANDERSEN_THERMOSTAT) { //! the Andersen Thermostat
				//global_log->info() << "Andersen Thermostat" << endl;
				double nuDt = _nuAndersen * _integrator->getTimestepLength();
				//global_log->info() << "Timestep length = " << _integrator->getTimestepLength() << " nuDt = " << nuDt << "\n";
				unsigned numPartThermo = 0; // for testing reasons
				double tTarget;
				double stdDevTrans, stdDevRot;
				if(_domain->severalThermostats()) {
					for (ParticleIterator tM = _moleculeContainer->iteratorBegin(); tM != _moleculeContainer->iteratorEnd(); ++tM) {
						if (_rand.rnd() < nuDt) {
							numPartThermo++;
							int thermostat = _domain->getThermostat(tM->componentid());
							tTarget = _domain->getTargetTemperature(thermostat);
							stdDevTrans = sqrt(tTarget/tM->gMass());
							for(unsigned short d = 0; d < 3; d++) {
								stdDevRot = sqrt(tTarget*tM->getI(d));
								tM->setv(d,_rand.gaussDeviate(stdDevTrans));
								tM->setD(d,_rand.gaussDeviate(stdDevRot));
							}
						}
					}
				}
				else{
					tTarget = _domain->getTargetTemperature(0);
					for (ParticleIterator tM = _moleculeContainer->iteratorBegin(); tM != _moleculeContainer->iteratorEnd(); ++tM) {
						if (_rand.rnd() < nuDt) {
							numPartThermo++;
							// action of the anderson thermostat: mimic a collision by assigning a maxwell distributed velocity
							stdDevTrans = sqrt(tTarget/tM->gMass());
							for(unsigned short d = 0; d < 3; d++) {
								stdDevRot = sqrt(tTarget*tM->getI(d));
								tM->setv(d,_rand.gaussDeviate(stdDevTrans));
								tM->setD(d,_rand.gaussDeviate(stdDevRot));
							}
						}
					}
				}
				//global_log->info() << "Andersen Thermostat: n = " << numPartThermo ++ << " particles thermostated\n";
			}

			/*
			if(_mirror && _applyMirror){
				_mirror->VelocityChange(_moleculeContainer, _domain);
			}
			*/

		}
		// mheinen 2015-07-27 --> TEMPERATURE_CONTROL
        else if ( _temperatureControl != NULL) {
            _temperatureControl->DoLoopsOverMolecules(_moleculeContainer, _simstep);
        }
        // <-- TEMPERATURE_CONTROL
		
		

		advanceSimulationTime(_integrator->getTimestepLength());

		/* BEGIN PHYSICAL SECTION:
		 * the system is in a consistent state so we can extract global variables
		 */
		/*
		ensemble.updateGlobalVariable(NUM_PARTICLES);
		global_log->debug() << "Number of particles in the Ensemble: " << ensemble.N() << endl;
		ensemble.updateGlobalVariable(ENERGY);
		global_log->debug() << "Kinetic energy in the Ensemble: " << ensemble.E() << endl;
		ensemble.updateGlobalVariable(TEMPERATURE);
		global_log->debug() << "Temperature of the Ensemble: " << ensemble.T() << endl;
		*/
		/* END PHYSICAL SECTION */

		computationTimer->stop();
		perStepIoTimer->start();

		output(_simstep);
		
		
		/*! by Stefan Becker <stefan.becker@mv.uni-kl.de> 
		  * realignment tools borrowed from Martin Horsch
		  * For the actual shift the halo MUST NOT be present!
		  */
		if(_doAlignCentre && !(_simstep % _alignmentInterval)) {
			_domain->realign(_moleculeContainer);
#ifndef NDEBUG 
#ifndef ENABLE_MPI
			unsigned particleNoTest = 0;
			particleNoTest = _moleculeContainer->getNumberOfParticles();
			cout <<"particles after realign(), halo absent: " << particleNoTest<< "\n";
#endif
#endif
		}
		
		if(_forced_checkpoint_time >= 0 && (decompositionTimer->get_etime() + computationTimer->get_etime()
				+ ioTimer->get_etime() + perStepIoTimer->get_etime()) >= _forced_checkpoint_time) {
			/* force checkpoint for specified time */
			string cpfile(_outputPrefix + ".timed.restart.xdr");
			global_log->info() << "Writing timed, forced checkpoint to file '" << cpfile << "'" << endl;
			_domain->writeCheckpoint(cpfile, _moleculeContainer, _domainDecomposition, _simulationTime);
			_forced_checkpoint_time = -1; /* disable for further timesteps */
		}
		perStepIoTimer->stop();
	}
	loopTimer->stop();
	/***************************************************************************/
	/* END MAIN LOOP                                                           */
	/*****************************//**********************************************/
    ioTimer->start();
    if( _finalCheckpoint ) {
        /* write final checkpoint */
        string cpfile(_outputPrefix + ".restart.xdr");
        global_log->info() << "Writing final checkpoint to file '" << cpfile << "'" << endl;
        _domain->writeCheckpoint(cpfile, _moleculeContainer, _domainDecomposition, _simulationTime, _finalCheckpointBinary);
    }
	// finish output
	std::list<OutputBase*>::iterator outputIter;
	for (outputIter = _outputPlugins.begin(); outputIter != _outputPlugins.end(); outputIter++) {
		(*outputIter)->finishOutput(_moleculeContainer, _domainDecomposition, _domain);
		delete (*outputIter);
	}
	ioTimer->stop();
	global_simulation->printTimers();
	global_simulation->resetTimers();
	_memoryProfiler->doOutput();
	global_log->info() << endl;

#if WITH_PAPI
	global_log->info() << "PAPI counter values for loop timer:"  << endl;
	for(int i = 0; i < loopTimer->get_papi_num_counters(); i++) {
		global_log->info() << "  " << papi_event_list[i] << ": " << loopTimer->get_global_papi_counter(i) << endl;
	}
#endif /* WITH_PAPI */

	unsigned long numTimeSteps = _numberOfTimesteps - _initSimulation + 1; // +1 because of <= in loop
	double elapsed_time = loopTimer->get_etime();
	if(NULL != _flopCounter) {
		double flop_rate = _flopCounter->getTotalFlopCount() * numTimeSteps / elapsed_time / (1024*1024);
		global_log->info() << "FLOP-Count per Iteration: " << _flopCounter->getTotalFlopCount() << " FLOPs" <<endl;
		global_log->info() << "FLOP-rate: " << flop_rate << " MFLOPS" << endl;
	}
}

void Simulation::output(unsigned long simstep) {

	int mpi_rank = _domainDecomposition->getRank();

	std::list<OutputBase*>::iterator outputIter;
	for (outputIter = _outputPlugins.begin(); outputIter != _outputPlugins.end(); outputIter++) {
		OutputBase* output = (*outputIter);
		global_log->debug() << "Output from " << output->getPluginName() << endl;
		output->doOutput(_moleculeContainer, _domainDecomposition, _domain, simstep, &(_lmu), &(_mcav));
	}

	if ((simstep >= _initStatistics) && _doRecordProfile && !(simstep % _profileRecordingTimesteps)) {
		_domain->recordProfile(_moleculeContainer, _doRecordVirialProfile);
	}
	if ((simstep >= _initStatistics) && _doRecordProfile && !(simstep % _profileOutputTimesteps)) {
		_domain->collectProfile(_domainDecomposition, _doRecordVirialProfile);
		if (mpi_rank == 0) {
			ostringstream osstrm;
			osstrm << _profileOutputPrefix << "." << fill_width('0', 9) << simstep;
			//edited by Michaela Heier 
			if(this->_domain->isCylindrical()){
				this->_domain->outputCylProfile(osstrm.str().c_str(),_doRecordVirialProfile);
				//_domain->outputProfile(osstrm.str().c_str(),_doRecordVirialProfile);
			}
			else{
			_domain->outputProfile(osstrm.str().c_str(), _doRecordVirialProfile);
			}
			osstrm.str("");
			osstrm.clear();
		}
		_domain->resetProfile(_doRecordVirialProfile);
	}

	
	if (_domain->thermostatWarning())
		global_log->warning() << "Thermostat!" << endl;
	/* TODO: thermostat */
	global_log->info() << "Simstep = " << simstep << "\tT = "
			<< _domain->getGlobalCurrentTemperature() << "\tU_pot = "
			<< _domain->getGlobalUpot() << "\tp = "
			<< _domain->getGlobalPressure() << endl;
}

void Simulation::finalize() {
	if (_FMM != NULL) {
		_FMM->printTimers();
		bhfmm::VectorizedLJP2PCellProcessor * temp = dynamic_cast<bhfmm::VectorizedLJP2PCellProcessor*>(_cellProcessor);
		temp->printTimers();
	}

	if (_domainDecomposition != NULL) {
		delete _domainDecomposition;
		_domainDecomposition = NULL;
	}
	global_simulation = NULL;
}

void Simulation::updateParticleContainerAndDecomposition() {
	// The particles have moved, so the neighborhood relations have
	// changed and have to be adjusted
	_moleculeContainer->update();
	//_domainDecomposition->exchangeMolecules(_moleculeContainer, _domain);
	bool forceRebalancing = false;
	global_simulation->startTimer("SIMULATION_MPI_OMP_COMMUNICATION");
	_domainDecomposition->balanceAndExchange(forceRebalancing, _moleculeContainer, _domain);
	global_simulation->stopTimer("SIMULATION_MPI_OMP_COMMUNICATION");
	// The cache of the molecules must be updated/build after the exchange process,
	// as the cache itself isn't transferred
	_moleculeContainer->updateMoleculeCaches();
}

void Simulation::performOverlappingDecompositionAndCellTraversalStep() {
	bool forceRebalancing = false;

	#ifdef ENABLE_MPI
		#ifdef ENABLE_OVERLAPPING
			NonBlockingMPIHandlerBase* nonBlockingMPIHandler =
					new NonBlockingMPIMultiStepHandler(static_cast<DomainDecompMPIBase*>(_domainDecomposition), _moleculeContainer, _domain, _cellProcessor);
		#else
			NonBlockingMPIHandlerBase* nonBlockingMPIHandler =
					new NonBlockingMPIHandlerBase(static_cast<DomainDecompMPIBase*>(_domainDecomposition), _moleculeContainer, _domain, _cellProcessor);
		#endif

		nonBlockingMPIHandler->performOverlappingTasks(forceRebalancing);
	#endif
}

void Simulation::setDomainDecomposition(DomainDecompBase* domainDecomposition) {
	if (_domainDecomposition != nullptr) {
		delete _domainDecomposition;
	}
	_domainDecomposition = domainDecomposition;
}

unsigned long Simulation::getTotalNumberOfMolecules() const {
	return _domain->N();
}

/* FIXME: we should provide a more general way of doing this */
double Simulation::Tfactor(unsigned long simstep) {
	double xi = (double) (simstep - _initSimulation) / (double) (_initCanonical - _initSimulation);
	if ((xi < 0.1) || (xi > 0.9))
		return 1.0;
	else if (xi < 0.3)
		return 15.0 * xi - 0.5;
	else if (xi < 0.4)
		return 10.0 - 20.0 * xi;
	else if (xi < 0.6)
		return 2.0;
	else
		return 4 - 10.0 * xi / 3.0;
}

void Simulation::initialize() {
	int ownrank = 0;
#ifdef ENABLE_MPI
	MPI_CHECK( MPI_Comm_rank(MPI_COMM_WORLD, &ownrank) );
#endif

	global_simulation = this;

	_finalCheckpoint = true;
	_finalCheckpointBinary = false;

        // TODO:
#ifndef ENABLE_MPI
	global_log->info() << "Initializing the alibi domain decomposition ... " << endl;
	_domainDecomposition = new DomainDecompBase();
#else
	global_log->info() << "Initializing the standard domain decomposition ... " << endl;
	if (_domainDecomposition != nullptr) {
		delete _domainDecomposition;
	}
	_domainDecomposition = (DomainDecompBase*) new DomainDecomposition();
#endif
	global_log->info() << "Initialization done" << endl;

	/*
	 * default parameters
	 */
	_cutoffRadius = 0.0;
	_LJCutoffRadius = 0.0;
	_numberOfTimesteps = 1;
	_outputPrefix = string("mardyn");
	_outputPrefix.append(gettimestring());

	/** @todo the following features should be documented */
	_doRecordProfile = false;
	_doRecordVirialProfile = false;
	_profileRecordingTimesteps = 7;
	_profileOutputTimesteps = 12500;
	_profileOutputPrefix = "out";
	_collectThermostatDirectedVelocity = 100;
	_initCanonical = _initSimulation + 1;  // default: simulate canonical (with Tfactor == 1.) from begin on!
	_initGrandCanonical = 10000000;
	_initStatistics = 20000;
	h = 0.0;

	_thermostatType = VELSCALE_THERMOSTAT;
	_nuAndersen = 0.0;
	_rand.init(8624);

	_doAlignCentre = false;
	_componentSpecificAlignment = false;
	_alignmentInterval = 25;
	_momentumInterval = 1000;
	_wall = NULL;
	_applyWallFun_LJ_9_3 = false;
	_applyWallFun_LJ_10_4 = false;
	_mirror = NULL;
	_applyMirror = false;

	_pressureGradient = new PressureGradient(ownrank);
	global_log->info() << "Constructing domain ..." << endl;
	_domain = new Domain(ownrank, this->_pressureGradient);
	global_log->info() << "Domain construction done." << endl;
	_particlePairsHandler = new ParticlePairs2PotForceAdapter(*_domain);
	_longRangeCorrection = NULL;
        
        this->_mcav = map<unsigned, CavityEnsemble>();
}
