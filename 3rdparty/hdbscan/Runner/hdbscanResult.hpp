#pragma once
#include <vector>
#include "../HdbscanStar/outlierScore.hpp"
class hdbscanResult
{
public:
	std::vector <int> labels;
    std::vector <outlierScore> outliersScores;
    std::vector <double> membershipProbabilities;
	bool hasInfiniteStability;
	hdbscanResult();
	hdbscanResult(std::vector<int> pLables, std::vector<outlierScore> pOutlierScores,
                  std::vector <double> pmembershipProbabilities, bool pHsInfiniteStability);
};

