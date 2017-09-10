#ifndef RDF_H
#define RDF_H

#include <cmath>
#include <string>
#include <vector>

#include "io/OutputBase.h"
#include "molecules/Molecule.h"

class Component;

/** @brief This class calculates the Radial Distribution Function (RDF).
 *
 * The RDF "describes how the atomic density varies as a function of the distance
 * from one particular atom." (see http://en.wikipedia.org/wiki/Radial_distribution_function ).
 * For example, it should be possible to recognize the aggregate state of a system
 * (see http://matdl.org/matdlwiki/index.php/softmatter:Radial_Distribution_Function ).
 *
 * \note The RDF is only determined for molecule pairs within the cutoff radius of the force and
 *       potential calculation. This means that bins outside the cut-off will always be computed
 *       to be zero.
 *
 * Calculation:
 * - calculate the distance of the pair, discretize it to intervalls
 * with length dr (i.e. sort the pairs into bins to obtain a histogram).
 * - for each bin: calculate the number density (i.e. number of particles per volume)
 *   of the corresponding shell
 * - divide the number density by the number density of the system.
 */
class RDF : public OutputBase {

	friend class RDFTest;

public:

	RDF();
	virtual ~RDF();

	/** @brief Read in XML configuration for RDFWriter.
	 *
	 * The following xml object structure is handled by this method:
	 * \code{.xml}
	   <outputplugin name="RDF">
	     <writefrequency>INTEGER</writefrequency>
	     <outputprefix>STRING</outputprefix>
	     <bins>INTEGER</bins>
	     <intervallength>DOUBLE</intervallength>
	   </outputplugin>
	   \endcode
	 */
	void readXML(XMLfileUnits& xmlconfig);

	void initOutput(ParticleContainer* particleContainer, DomainDecompBase* domainDecomp, Domain* domain);

	void finishOutput(ParticleContainer* particleContainer, DomainDecompBase* domainDecomp, Domain* domain);

	std::string getPluginName() {
		return std::string("RDF");
	}
	static OutputBase* createInstance() { return new RDF(); }

	//! @todo put this in the constructor (when the transition to the xml file is done),
	//! or create a seperate output component.
	void setOutputTimestep(unsigned int timestep) { _writeFrequency = timestep; }

	//! @todo put this in the constructor (when the transition to the xml file is done),
	//! or create a seperate output component.
	void setOutputPrefix(std::string prefix) { _outputPrefix = prefix; }

	//! plot all the statistics calculated to one or several files
	void doOutput(ParticleContainer* particleContainer, DomainDecompBase* domainDecomposition, Domain* domain, unsigned long simStep, std::list<ChemicalPotential>* lmu, std::map<unsigned, CavityEnsemble>* mcav);

	//! increment the counter indicating for how many iterations
	//! the molecule pairs have been counted.
	void tickRDF() {
		_numberOfRDFTimesteps++;
	}

	//! count the number of molecules per component
	//! @todo: remove it and replace it by component.getNumMolecules()
	void accumulateNumberOfMolecules(std::vector<Component>& components) const;

	void observeRDF(Molecule const& mi, Molecule const& mj, double dd) const {
		observeRDF(dd, mi.componentid(), mj.componentid());

		if(isEnabledSiteRDF()) {
			double drs[3];
			double dr2;
			unsigned si = mi.numSites();
			unsigned sj = mj.numSites();
			if(si+sj > 2) {
				for(unsigned m = 0; m < si; m++) {
					for(unsigned n = 0; n < sj; n++) {
						const std::array<double,3> dii = mi.site_d_abs(m);
						const std::array<double,3> djj = mj.site_d_abs(n);
						SiteSiteDistanceAbs(dii.data(), djj.data(), drs, dr2);
						observeRDF(dr2, mi.componentid(), mj.componentid(), m, n);
					}
				}
			}
		}
	}

	/**
	 * This method "really" counts the number of molecule pairs within a certain distance.
	 */
	void observeRDF(double dd, unsigned i, unsigned j) const {
		if(dd > _maxDistanceSquare) return;
		if(i > j) {
			this->observeRDF(dd, j, i);
			return;
		}

		unsigned l = (unsigned)floor(sqrt(dd)/this->_intervalLength);
		#if defined _OPENMP
		#pragma omp atomic
		#endif
		this->_localDistribution[i][j-i][l] ++;
	}

	/**
	 * Count center pairing for particle pair for molecules i and j and centers
	 * m_i, n_j at distance dd.
	 */
	inline void observeRDF(double dd, unsigned i, unsigned j, unsigned m, unsigned n) const {
		if(dd > _maxDistanceSquare) return;
		if(i > j) {
			this->observeRDF(dd, j, i, n, m);
			return;
		}

		unsigned l = (unsigned)floor(sqrt(dd)/this->_intervalLength);
		#if defined _OPENMP
		#pragma omp atomic
		#endif
		this->_localSiteDistribution[i][j-i][m][n][l] ++;
		if((i == j) && (m != n)){
			#if defined _OPENMP
			#pragma omp atomic
			#endif
			this->_localSiteDistribution[i][j-i][n][m][l] ++;
		}
	}

	bool isEnabledSiteRDF() const { return _doCollectSiteRDF; }

	void reset();  //!< reset all values to 0, except the accumulated ones.

private:
	void init();
	unsigned int numBins() const { return _bins; }
	double binwidth() const { return _intervalLength; }
	void collectRDF(DomainDecompBase* domainDecomp);  //!< update global values from local once

	//! Update the "accumulatedXXX"-fields from the "global"-variables.
	//! @note consequently, collectRDF should be called just before.
	void accumulateRDF();

	void writeToFile(const Domain* domain, std::string filename, unsigned int i, unsigned int j) const;

	//! The length of an interval
	//! Only used for the output to scale the "radius"-axis.
	double _intervalLength;

	//! The number of bins, i.e. the number of intervals in which the cutoff
	//! radius will be subdivided.
	unsigned int _bins;

	//! number of different components (i.e. molecule types).
	unsigned int _numberOfComponents;

	//! components vector
	std::vector<Component>* _components;

	//! number of timesteps over which the counters are being accumulated
	//! since the last calculation of the RDF.
	int _numberOfRDFTimesteps;

	//! number of timesteps over which the "accumulated"-counters are being
	//! accumulated
	int _accumulatedNumberOfRDFTimesteps;

	//! the maximum distance up to which particle pairs are counted, squared
	double _maxDistanceSquare;

	/**
	 * holds the numberOfMolecules of component i at _globalCtr[i], globally.
	 * accumulates over time steps as the number of molecules may change
	 */
	unsigned long* _globalCtr;

	//! holds the numberOfMolecules of component i at _globalAccumulatedCtr[i],
	//! globally and accumulated over all timesteps for which particles where
	//! counted for the RDF.
	unsigned long* _globalAccumulatedCtr;

	//! holds the distribution of the neighbouring particles, locally for this process,
	//! i.e. the number of particles of components m and n in bin b: _localDistribution[m][n][b];
	unsigned long ***_localDistribution;

	//! holds the distribution of the neighbouring particles, globally.
	unsigned long ***_globalDistribution;

	//! holds the distribution of the neighbouring particles, globally accumulated.
	unsigned long ***_globalAccumulatedDistribution;

	bool _doCollectSiteRDF;

	unsigned long *****_localSiteDistribution;

	unsigned long *****_globalSiteDistribution;

	unsigned long *****_globalAccumulatedSiteDistribution;

	unsigned int _writeFrequency;  //!< aggregation and output writing interval for the RDF data
	std::string _outputPrefix;  //!< output prefix for rdf files

	bool _initialized;
	bool _readConfig;
};

#endif /* RDF_H */
