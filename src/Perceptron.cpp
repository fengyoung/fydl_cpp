#include "Perceptron.h"
#include "Timer.h"
#include "StringArray.h"
#include "Activation.h"
#include "ConfigFile.h"
using namespace fydl; 
#include <algorithm>
#include <fstream>
#include <iostream>
using namespace std; 
#include <math.h>
#include <stdio.h>


////////////////////////////////////////////////////////////////////////////////////////////
// Construction & Destruction  

Perceptron::Perceptron()
{
	m_ai = NULL; 
	m_ao = NULL; 

	m_do = NULL; 
}


Perceptron::~Perceptron()
{
	Release(); 
}


////////////////////////////////////////////////////////////////////////////////////////////
// Operations 

void Perceptron::Init(const PerceptronParamsT perceptronParamsT, const PerceptronLearningParamsT perceptronLearningParamsT)
{
	Release(); 

	m_paramsPerceptron = perceptronParamsT;
	m_paramsPerceptron.input += 1;	// add 1 for bias nodes 
	m_paramsLearning = perceptronLearningParamsT; 

	Create(); 
}


bool Perceptron::InitFromConfig(const char* sConfigFile, const int32_t nInput, const int32_t nOutput)
{
	ConfigFile conf_file; 
	if(!conf_file.Read(sConfigFile)) 
		return false; 
	Release(); 

	m_paramsPerceptron.input = nInput + 1;	// add 1 for bias nodes
	m_paramsPerceptron.output = nOutput; 
	m_paramsPerceptron.act_output = TypeDefs::ActType(conf_file.GetVal_asString("Activation").c_str());   
	if(m_paramsPerceptron.act_output == _ACT_NONE)
		return false; 

	m_paramsLearning.regula = TypeDefs::RegulaType(conf_file.GetVal_asString("Regula").c_str()); 
	m_paramsLearning.mini_batch = conf_file.GetVal_asInt("MiniBatch"); 
	m_paramsLearning.iterations = conf_file.GetVal_asInt("Iterations"); 
	m_paramsLearning.learning_rate = conf_file.GetVal_asFloat("LearningRate"); 
	m_paramsLearning.rate_decay = conf_file.GetVal_asFloat("RateDecay"); 
	m_paramsLearning.epsilon = conf_file.GetVal_asFloat("Epsilon"); 

	Create(); 

	return true; 
}


void Perceptron::Train(vector<Pattern*>& vtrPatts)
{
	int32_t cross_cnt = (int32_t)vtrPatts.size() / 20;			// 5% patterns for corss validation
	int32_t train_cnt = (int32_t)vtrPatts.size() - cross_cnt;	// 95% patterns for training
	double learning_rate = m_paramsLearning.learning_rate;	// learning rate, it would be update after every iteration
	double error, rmse;	// training error and RMSE in one iteration
	pair<double,double> validation;	// precision and RMSE of validation 
	Timer timer;		// timer

	// create assistant variables for training
	CreateAssistant();
	
	// shuffle pattens 
	random_shuffle(vtrPatts.begin(), vtrPatts.end());

	for(int32_t t = 0; t < m_paramsLearning.iterations; t++)
	{
		error = 0.0; 	

		timer.Start(); 	

		// shuffle training patterns
		random_shuffle(vtrPatts.begin(), vtrPatts.end() - cross_cnt);

		for(int32_t p = 0; p < train_cnt; p++) 
		{
			// forward & backward phase
			FeedForward(vtrPatts[p]->m_x, vtrPatts[p]->m_nXCnt); 
			error += BackPropagate(vtrPatts[p]->m_y, vtrPatts[p]->m_nYCnt); 
			m_nPattCnt++; 	

			if(m_paramsLearning.mini_batch > 0)	// online or mini-batch
			{
				if(m_nPattCnt >= m_paramsLearning.mini_batch) 
					ModelUpdate(learning_rate); 
			}
		}	

		if(m_paramsLearning.mini_batch == 0)	// batch 
			ModelUpdate(learning_rate); 

		validation = Validation(vtrPatts, cross_cnt); 
		rmse = sqrt(error / (double)(train_cnt));

		timer.Stop(); 	

		printf("iter %d | learning_rate: %.6g | error: %.6g | rmse: %.6g | validation(pr & rmse): %.4g%% & %.6g | time_cost(s): %.3f\n", 
				t+1, learning_rate, error, rmse, validation.first * 100.0, validation.second, timer.GetLast_asSec()); 
		learning_rate = learning_rate * (learning_rate / (learning_rate + (learning_rate * m_paramsLearning.rate_decay)));	

		if(rmse <= m_paramsLearning.epsilon)
			break;
	}
}


int32_t Perceptron::Predict(double* y, const int32_t y_len, const double* x, const int32_t x_len)
{
	if(!y || !x)
		return _FYDL_ERROR_INPUT_NULL;
	if(y_len != m_paramsPerceptron.output || x_len != m_paramsPerceptron.input - 1)
		return _FYDL_ERROR_WRONG_LEN;
	if(m_wo.IsNull())
		return _FYDL_ERROR_MODEL_NULL;

	// for thread save, do not use inner layer
	double* ai = new double[m_paramsPerceptron.input];		// input layer
	
	// activate input layer
	for(int32_t i = 0; i < x_len; i++) 
		ai[i] = x[i];
	ai[x_len] = 1.0;		// set bias

	// activate output 
	double sum; 	
	double e = 0.0; 
	for(int32_t j = 0; j < m_paramsPerceptron.output; j++)	
	{
		sum = 0.0; 	
		for(int32_t i = 0; i < m_paramsPerceptron.output; i++)
			sum += ai[i] * m_wo[i][j];
		y[j] = Activation::Activate(sum, m_paramsPerceptron.act_output);  

		if(m_paramsPerceptron.act_output == _ACT_SOFTMAX)
			e += y[j]; 
	}
	if(m_paramsPerceptron.act_output == _ACT_SOFTMAX)
	{
		for(int32_t j = 0; j < m_paramsPerceptron.output; j++)	
			y[j] /= e; 
	}

	delete ai; 
	
	return _FYDL_SUCCESS; 
}


int32_t Perceptron::Save(const char* sFile, const char* sTitle)
{
	if(m_wo.IsNull())
		return _FYDL_ERROR_MODEL_NULL; 
	ofstream ofs(sFile); 
	if(!ofs.is_open())
		return _FYDL_ERROR_FILE_OPEN;

	ofs<<"** "<<sTitle<<" **"<<endl; 
	ofs<<endl;

	// save learning parameters
	ofs<<"@learning_params"<<endl; 
	TypeDefs::Print_PerceptronLearningParamsT(ofs, m_paramsLearning); 
	ofs<<endl; 
	
	// save architecture parameters of RBM
	ofs<<"@architecture_params"<<endl; 
	TypeDefs::Print_PerceptronParamsT(ofs, m_paramsPerceptron); 
	ofs<<endl; 

	// save transtorm matrix
	ofs<<"@weight"<<endl; 
	Matrix::Print_Matrix(ofs, m_wo);
	ofs<<endl; 

	ofs.close(); 
	return _FYDL_SUCCESS; 
}


int32_t Perceptron::Load(const char* sFile, const char* sCheckTitle)
{
	ifstream ifs(sFile);  
	if(!ifs.is_open())
		return _FYDL_ERROR_FILE_OPEN;
	Release(); 

	string str; 

	if(sCheckTitle)
	{
		std::getline(ifs, str); 
		if(str.find(sCheckTitle) == string::npos) 
		{
			ifs.close();
			return _FYDL_ERROR_NOT_MODEL_FILE;
		}
	}

	while(!ifs.eof())
	{
		std::getline(ifs, str);
		if(str.empty())
			continue; 
		else if(str == "@learning_params")
		{
			if(!TypeDefs::Read_PerceptronLearningParamsT(m_paramsLearning, ifs))
				return _FYDL_ERROR_LERANING_PARAMS;
		}
		else if(str == "@architecture_params")
		{
			if(!TypeDefs::Read_PerceptronParamsT(m_paramsPerceptron, ifs))
				return _FYDL_ERROR_ACH_PARAMS;
			Create(); 
		}
		else if(str == "@weight")
		{
			if(!Matrix::Read_Matrix(m_wo, ifs))
				return _FYDL_ERROR_MODEL_DATA;
		}
	}

	ifs.close();
	return _FYDL_SUCCESS; 
}


PerceptronLearningParamsT Perceptron::GetLearningParams()
{
	return m_paramsLearning; 
}


PerceptronParamsT Perceptron::GetArchParams()
{
	return m_paramsPerceptron;	
}


////////////////////////////////////////////////////////////////////////////////////////////
// Internal Operations


void Perceptron::Create()
{
	// create transform matrix
	m_wo.Create(m_paramsPerceptron.input, m_paramsPerceptron.output); 
	Activation::InitTransformMatrix(m_wo, m_paramsPerceptron.act_output); 
	
	m_nPattCnt = 0; 
}


void Perceptron::Release()
{
	ReleaseAssistant();
}
	

void Perceptron::CreateAssistant()
{
	// create change matrix
	m_co.Create(m_paramsPerceptron.input, m_paramsPerceptron.output); 
	m_co.Init(0.0);

	// create input layer
	m_ai = new double[m_paramsPerceptron.input];
	// create output layer
	m_ao = new double[m_paramsPerceptron.output];

	// create delta array
	m_do = new double[m_paramsPerceptron.output];
	
	m_nPattCnt = 0; 
}


void Perceptron::ReleaseAssistant()
{
	if(m_ai)
	{
		delete m_ai; 
		m_ai = NULL; 
	}
	if(m_ao)
	{
		delete m_ao; 
		m_ao = NULL; 
	}

	if(m_do)
	{
		delete m_do; 
		m_do = NULL; 
	}
}


void Perceptron::FeedForward(const double* in_vals, const int32_t in_len)
{
	if(!in_vals || in_len != m_paramsPerceptron.input - 1)
		throw "Perceptron::FeedForward() ERROR: Wrong length of \'in_vals\'!"; 

	// activate input layer
	for(int32_t i = 0; i < in_len; i++) 
		m_ai[i] = in_vals[i];
	m_ai[in_len] = 1.0;		// set bias

	// activate output layer
	double sum; 	
	double e = 0.0; 
	for(int32_t j = 0; j < m_paramsPerceptron.output; j++)	
	{
		sum = 0.0; 	
		for(int32_t i = 0; i < m_paramsPerceptron.output; i++)
			sum += m_ai[i] * m_wo[i][j];
		m_ao[j] = Activation::Activate(sum, m_paramsPerceptron.act_output);  

		if(m_paramsPerceptron.act_output == _ACT_SOFTMAX)
			e += m_ao[j]; 
	}
	if(m_paramsPerceptron.act_output == _ACT_SOFTMAX)
	{
		for(int32_t j = 0; j < m_paramsPerceptron.output; j++)	
			m_ao[j] /= e; 
	}
}


double Perceptron::BackPropagate(const double* out_vals, const int32_t out_len)
{
	if(!out_vals || out_len != m_paramsPerceptron.output)
		throw "Perceptron::BackPropagate() ERROR: Wrong length of \'out_vals\'!"; 

	double error = 0.0; 
	// calculate delta and error of output 
	for(int32_t j = 0; j < m_paramsPerceptron.output; j++) 
	{
		m_do[j] = m_ao[j] - out_vals[j]; 
		error += m_do[j] * m_do[j];  
	}

	// update change matrix
	for(int32_t j = 0; j < m_paramsPerceptron.output; j++)
	{ // m_co
		for(int32_t i = 0; i < m_paramsPerceptron.input; i++) 
			m_co[i][j] += m_do[j] * Activation::DActivate(m_ao[j], m_paramsPerceptron.act_output) * m_ai[i]; 
	}
	
	return error / double(m_paramsPerceptron.output); 
}


void Perceptron::ModelUpdate(const double learning_rate)
{
	// update transform matrix
	for(int32_t i = 0; i < m_paramsPerceptron.input; i++) 
	{
		for(int32_t j = 0; j < m_paramsPerceptron.output; j++) 
		{
			m_wo[i][j] -= learning_rate * (m_co[i][j] / (double)m_nPattCnt + Activation::DActRegula(m_wo[i][j], m_paramsLearning.regula));
			m_co[i][j] = 0.0; 
		}
	}
	
	m_nPattCnt = 0; 
}


pair<double, double> Perceptron::Validation(vector<Pattern*>& vtrPatts, const int32_t nBackCnt)
{
	double error = 0.0; 
	int32_t correct = 0, total = 0; 
	int32_t patts = (int32_t)vtrPatts.size(); 
	double* y = new double[m_paramsPerceptron.output];
	int32_t k; 

	for(int32_t i = 0; i < nBackCnt && i < patts; i++) 
	{
		k = patts-1-i;
		Predict(y, m_paramsPerceptron.output, vtrPatts[k]->m_x, vtrPatts[k]->m_nXCnt); 
		if(Pattern::MaxOff(y, m_paramsPerceptron.output) == Pattern::MaxOff(vtrPatts[k]->m_y, vtrPatts[k]->m_nYCnt))
			correct += 1; 	
		error += Pattern::Error(y, vtrPatts[k]->m_y, m_paramsPerceptron.output);  	
		total += 1; 	
	}

	delete y;

	double pr = (double)correct / (double)total;
	double rmse = sqrt(error / (double)total);

	return pair<double,double>(pr, rmse); 
}


