#include "LPSolver.hpp"

#include <chrono>
#include <iostream>

#include <sys/time.h>

#define PSD_EIGEN_TOL 0.00001
#define MAX_TRIES_ROUNDING 20
#define CONSTRAINT_FAIL_LIMIT 5
#define CONSTRAINT_REMOVAL_SLACK 0.99

typedef unsigned long long timestamp_t;

static timestamp_t
get_timestamp ()
{
  struct timeval now;
  gettimeofday (&now, NULL);
  return  now.tv_usec + (timestamp_t)now.tv_sec * 1000000;
}

//Construct solver
LPSolver::LPSolver(Problem* p) {
	problem = p;
	nQP = p->nQP;
	nLP = nQP*(nQP-1)/2;
	
	//Establish an initial lower bound through guessing the naive all-one vector
	bestSol = VectorXd(nQP);
	bestSol.setOnes();
	lowerBound = p->score(bestSol);
	
	//Establish an initial upper bound by summing abs of each coefficient
	upperBound = p->constantTerm;
	for(uint32_t i=0;i<nQP;i++){
		for(uint32_t j=i+1;j<nQP;j++){
			upperBound += fabs(p->coeffs(i,j));
		}
	}
	
	//Create GLPK instance
	lp = glp_create_prob();
#ifdef USE_INTERIOR
	glp_init_iptcp(&parm);
#else
	glp_init_smcp(&parm);
#endif
	parm.msg_lev = GLP_MSG_ERR;
	
	//Allocate room for LP solution
	currSol = std::vector<float>(nLP); 
	
	glp_set_prob_name(lp, "CLQO"); //Name the problem
	glp_set_obj_dir(lp, GLP_MAX);  //We maximize
	
	//each column corresponds to a variable we (linearly) optimize over
	glp_add_cols(lp, nLP);
	for(uint32_t i=1;i<=nLP;i++){
		//Each variable has DB (double bound) to [-1,+1]
		glp_set_col_bnds(lp, i, GLP_DB, -1., 1.);
		std::pair<int,int> xy = getQPVars(i);
		
		//Set objective weight
		glp_set_obj_coef(lp, i, p->coeffs(xy.second, xy.first));
	}
	
	active_clauses = std::vector<constraint>();
}

//Given a variable vi and vj, get the index of vij
uint32_t LPSolver::getLPVar(uint32_t x, uint32_t y){
	if(y == x) throw std::runtime_error(std::string("Bad LP: ")+std::to_string(x)+", "+std::to_string(y));
	if(y > x) return getLPVar(y, x);
	if(y >= nQP || x >= nQP) throw std::runtime_error(std::string("Bad LP: ")+std::to_string(x)+", "+std::to_string(y)+", "+std::to_string(nQP));
	return 1 + y + x*(x-1)/2;
}

//given vij, find i and j
std::pair<uint32_t,uint32_t> LPSolver::getQPVars(uint32_t v){
	if(v > nLP) throw std::runtime_error(std::string("Bad vQP: ")+std::to_string(v)+", "+std::to_string(nLP));
	
	uint32_t x = floor(sqrt(2*v)+1./2);
	uint32_t y = v - 1 - x*(x-1)/2;
	
	if(y >= x) throw std::runtime_error(std::string("Bad QP: ")+std::to_string(x)+", "+std::to_string(y)+", "+std::to_string(v));
	
	return std::pair<uint32_t,uint32_t>(x,y);
}



//Given an assignment, find a minimal (but non necess. minimum) set of
//rows/columns that has a constraint that's it violating.
//If it's actually positive semidefinite, and there are not constraints
//that it's violating, return the empty list. (Congrats! This means you
//found the global optimum!) 
std::vector<uint32_t>& LPSolver::nonPSDcore(){
	//Two phases: one where we add rows hoping to make it not PSD,
	//and then later we remove rows to make it minimal.
	
	//Rows we're currently taking
	std::vector<uint32_t>& core = *new std::vector<uint32_t>();
	//Rows we have not taken
	std::vector<uint32_t> notInCore;
	for(uint32_t i=0;i<nQP;i++) notInCore.push_back(i);
	
	//put the rows in a random order
	std::random_shuffle(notInCore.begin(), notInCore.end());
	
	do{
		uint32_t newRow = notInCore.back();
		notInCore.pop_back();
		core.push_back(newRow);
		
		MatrixXd& subMat = getSubmatrix(core);
		//std::cout << subMat << std::endl;
		
		Eigen::SelfAdjointEigenSolver<MatrixXd> eig(subMat);
		auto evals = eig.eigenvalues();
		bool isPSD = true;
		for(uint32_t i=0; i<evals.size(); i++){
			if(evals[i] < -PSD_EIGEN_TOL){
				isPSD = false;
				break;
			}
		}
		delete &subMat;
		
		if(isPSD){
			if(notInCore.size() > 0)
				continue; //add more
			else {
				core.clear();
				return core; //nothing nonPSD, return empty vector
			}
		} else {
			//we found a non-PSD chunk!
			break;
		}
		
	}while(true);
	
	uint32_t largeMatSize = core.size();
	for(uint32_t iter=0; iter<largeMatSize; iter++){
		uint32_t removedRow = core[0];
		//std::cout << "Pared down " << removedRow << std::endl;
		core.erase(core.begin());
		
		MatrixXd& subMat = getSubmatrix(core);
		//std::cout << subMat << std::endl;
		
		Eigen::SelfAdjointEigenSolver<MatrixXd> eig(subMat);
		auto evals = eig.eigenvalues();
		bool isPSD = true;
		for(uint32_t i=0; i<evals.size(); i++){
			if(evals[i] < -PSD_EIGEN_TOL){
				isPSD = false;
				break;
			}
		}
		delete &subMat;
		
		if(isPSD) //the row we removed was necessary for non-PSD-ness, add it back in
			core.push_back(removedRow);
		//else great, we didn't need it. do nothing and repeat.
	}
	
	return core;
}

MatrixXd& LPSolver::getSubmatrix(std::vector<uint32_t> rows){
	
	MatrixXd& result = *new MatrixXd(rows.size(), rows.size());
	for(uint32_t i=0;i<rows.size();i++){
		result(i,i) = 1;
		
		for(uint32_t j=i+1;j<rows.size();j++)
			result(j,i) = result(i,j) = currSol[getLPVar(rows[i],rows[j])-1];
	}
	
	return result;
}

MatrixXd& LPSolver::getMatrix(){
	MatrixXd& result = *new MatrixXd(nQP, nQP);
	for(uint32_t i=0;i<nQP;i++){
		result(i,i) = 1;
		
		for(uint32_t j=i+1;j<nQP;j++)
			result(j,i) = result(i,j) = currSol[getLPVar(i,j)-1];
	}
	
	return result;
}

float LPSolver::scoreRelaxation(){
	float score = problem->constantTerm;
	for(uint32_t i=0;i<nQP;i++){
		for(uint32_t j=i+1;j<nQP;j++)
			score += currSol[getLPVar(i,j)-1] * problem->coeffs(i,j);
	}
	return score;
}

void LPSolver::roundToSol(){//TODO write the solution to the problem
	
	//TODO call to an actual SDP solver
	
	MatrixXd& solMat = getMatrix();
	
	std::cout << "The matrix Sol:" << std::endl << solMat << std::endl;
	
	//for now --
	//Reduce 'magnitude' slightly to make PSD
	Eigen::SelfAdjointEigenSolver<MatrixXd> eig(solMat);
	auto evals = eig.eigenvalues();
	double minEigenvalue = evals[0] - 0.00001;
	
	solMat *= -1. / (-1 + minEigenvalue);
	solMat += MatrixXd::Identity(nQP, nQP) * (minEigenvalue / (-1 + minEigenvalue));
	
	std::cout << "SDP-ified Sol:" << std::endl;
	
	std::cout << solMat << std::endl;
	Eigen::LLT<MatrixXd> lltOfA(solMat); // compute the Cholesky decomposition of A
	MatrixXd L = lltOfA.matrixL(); // retrieve factor L  in the decomposition
	// The previous two lines can also be written as "L = A.llt().matrixL()"
	std::cout << "The Cholesky factor L is" << std::endl << L << std::endl;
	
	delete &solMat;
	
	for(int tries=0; tries<MAX_TRIES_ROUNDING; tries++){
		//generate random dot vector
		VectorXd v(nQP);
		std::default_random_engine generator(std::chrono::high_resolution_clock::now().time_since_epoch().count());
		std::normal_distribution<double> distribution(0.0,1.0);
		for(uint32_t i=0;i<nQP;i++){
			v(i) = distribution(generator);
			if(i==0) v(i) = fabs(v(i)); //normalize solution to start with "1"
		}
		
		VectorXd resultVec = L * v;
		//std::cout << "Dot product vector" << resultVec.transpose() << std::endl;
		for(uint32_t i=0;i<nQP;i++)
			resultVec(i) = std::copysign(1.0,resultVec(i));
		
		/*std::cout << "Final assignment: " << std::endl;
		for(uint32_t i=0;i<nQP;i++)
			printf("%f, ", resultVec(i));
		puts("");*/
		
		printf("Score = %f\n", problem->score(resultVec));
	}
}

void LPSolver::solve(){
	int constraintsEver = 0;
	int succConstraintFail = 0;
	
	double coreFindTime = 0.0, coreFindTotal = 0.0, simplexTime = 0.0, simplexTotal = 0.0;
	
solve:
	/* solve problem */
	timestamp_t simplexTimeStart = get_timestamp();
	int simplex_err = 
#ifdef USE_INTERIOR
	glp_interior(lp, &parm);
#else
	glp_simplex(lp, &parm);
#endif
	timestamp_t simplexTimeEnd = get_timestamp();
	simplexTotal += simplexTime = (simplexTimeEnd - simplexTimeStart) / 1000000.0L;
	if(simplex_err != 0) {
		printf("FAILED Error Code = %d\n", simplex_err);
		if(simplex_err == GLP_EINSTAB){
			printf("'just' an interior point stability check failed; using last point.\n");
		} else {
			exit(1);
		}
	}
#ifndef USE_INTERIOR
	if(glp_get_status(lp) != GLP_OPT) {
		printf("Simplex Optimality FAILED");
		exit(2);
	}
#endif
	
	for(uint32_t i=1;i<=nLP;i++){
	#ifdef USE_INTERIOR
		currSol[i-1] = glp_ipt_col_prim(lp, i);//glp_get_col_prim(lp, i);
	#else
		currSol[i-1] = glp_get_col_prim(lp, i);
	#endif
	}
	upperBound = std::min(upperBound, scoreRelaxation());
	printf("New bound: %f\n", upperBound);
	
findcore:
	timestamp_t timeCoreStart = get_timestamp();
	std::vector<uint32_t> core = nonPSDcore();
	if(core.size() == 0){
		std::cout << "Perfect core!";
		goto complete;
	} else {
		//We have a core, identify a new constraint
		MatrixXd coreMat = getSubmatrix(core);
		std::vector<double>& constraint = findConstraint(coreMat);
		timestamp_t timeCoreEnd = get_timestamp();
		coreFindTotal += coreFindTime = (timeCoreEnd - timeCoreStart) / 1000000.0L;
		if(constraint.size() == 0){
			succConstraintFail++;
			if(succConstraintFail == CONSTRAINT_FAIL_LIMIT)
				goto rounding;
			else
				goto findcore;
		} else {
			succConstraintFail=0;
			
			//for(uint32_t i=1;i<=nLP;i++) printf("%f, ",glp_get_col_prim(lp, i)); printf("\n");
			
			//Remove non-binding constraints. Could return but unlikely
			uint32_t rowNum = glp_get_num_rows(lp);
			for(int i=rowNum;i>0;i--){
				double slack = glp_get_row_prim(lp,i)-glp_get_row_lb(lp,i);
				if(slack > CONSTRAINT_REMOVAL_SLACK){
					printf("Deleting %d of %d, slack=%f\n", i, rowNum, slack);
					glp_del_rows(lp, 1, (const int*)&((&i)[-1]));
				}
			}
			
			//apply the constraint
			//constraint consists of linear terms on the
			//(core.size() choose 2) variables, preceded by
			//a constant term. The variables are in the submatrix,
			//and they'll need to mapped back "up" to the full matrix:
			//turn ij into i and j, and then map i and j to the larger matrix,
			//and then map back down to ij.
			glp_add_rows(lp, 1);
			rowNum = glp_get_num_rows(lp);
			
			printf("Applying constraint %d (%d)-- coreTime = %f, simplexTime = %f (this: %f, %f)\n", rowNum, constraintsEver, coreFindTotal, simplexTotal, coreFindTime, simplexTime);
			//build indices to pass to GLPK's sparse representation
			uint32_t indices[constraint.size()]; //size+1 for 1 indexing, (size+1)-1 for ignoring constant
			for(uint32_t v=0; v<constraint.size()-1; v++){
				std::pair<uint32_t,uint32_t> ij = getQPVars(1+v);
				uint32_t largeI = core[ij.first], largeJ = core[ij.second];
				uint32_t largeIJ = getLPVar(largeI, largeJ);
				indices[1+v] = largeIJ;
			}
			glp_set_mat_row(lp, rowNum, constraint.size()-1, (const int*)indices, &(constraint[0]));
			constraintsEver++;
			//sum[ coeff[i]*x[i] ] >= coeff[-1]
			glp_set_row_bnds(lp, rowNum, GLP_LO, constraint[0], 0.0);
			goto solve;
		}
		
		
	rounding:
		puts("No more constraints detected. Rounding off.");
		roundToSol();
		return;
	}
	
complete:
	puts("Global optimum found! Solution:");
	bestSol(0) = 1;
	for(uint32_t i=1;i<nQP;i++){
		bestSol(i) = lround(currSol[getLPVar(0,i)-1]);
	}
	//bestSol(0) = 1; bestSol(1) = -1; bestSol(2) = -1; bestSol(3) = -1; bestSol(4) = 1;
	std::cout << bestSol;
	
	{
		float score = problem->score(bestSol);
		printf("Score = %f\n", score);
	}
	
	return;
}

LPSolver::~LPSolver(){
	glp_delete_prob(lp);
	glp_free_env();
}