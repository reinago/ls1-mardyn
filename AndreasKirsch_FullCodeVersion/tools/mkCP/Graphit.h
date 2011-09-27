/*
 * GNU GPL version 2
 */

#include <map>
using namespace std;

#define SIG_WANG 6.2860
#define EPS_WANG 6.909e-05
#define ATOMIC_MASS_C 0.0120107
#define DEFAULT_A 2.6853
#define DEFAULT_A_ORIGINAL 2.7666
#define Z 6.33

#define TERSOFF_A 51.214
#define TERSOFF_B 12.740
#define TERSOFF_LAMBDA 1.8982
#define TERSOFF_LAMBDA_ORIG 1.8457
#define TERSOFF_MU 1.2039
#define TERSOFF_MU_ORIG 1.1705
#define TERSOFF_R 3.78
#define TERSOFF_R_ORIG 3.40
#define TERSOFF_S 4.44
#define TERSOFF_S_ORIG 3.97
#define TERSOFF_C 38049
#define TERSOFF_D 4.384
#define TERSOFF_H -0.57058
#define TERSOFF_N 0.72751
#define TERSOFF_BETA 1.5724e-07

class Graphit
{
 public:
   int getNumberOfAtoms();

   void calculateCoordinatesOfAtoms(
      int numberOfLayers, double xLength, double zLength, double A
   );
   double getX(int number);
   double getY(int number);
   double getZ(int number);

   void calculateVelocities(double T, double U);
   double getVelocityX(int number);
   double getVelocityY(int number);
   double getVelocityZ(int number);

   void reset();

 private:
   int numberOfAtoms;
   map<unsigned, double> coordinatesOfAtoms[3];
   map<unsigned, double> velocitiesOfAtoms[3];
};
