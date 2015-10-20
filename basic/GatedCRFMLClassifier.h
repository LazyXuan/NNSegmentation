/*
 * GatedCRFMLClassifier.h
 *
 *  Created on: Mar 18, 2015
 *      Author: mszhang
 */

#ifndef SRC_GatedCRFMLClassifier_H_
#define SRC_GatedCRFMLClassifier_H_

#include <iostream>

#include <assert.h>
#include "Example.h"
#include "Feature.h"
#include "Metric.h"
#include "N3L.h"

using namespace nr;
using namespace std;
using namespace mshadow;
using namespace mshadow::expr;
using namespace mshadow::utils;

//A native neural network classfier using only word embeddings
template<typename xpu>
class GatedCRFMLClassifier {
public:
  GatedCRFMLClassifier() {
    _dropOut = 0.5;
  }
  ~GatedCRFMLClassifier() {

  }

public:
  LookupTable<xpu> _words;
  // tag variables
  int _tagNum;
  int _tag_outputSize;
  vector<int> _tagSize;
  vector<int> _tagDim;
  NRVec<LookupTable<xpu> > _tags;

  int _wordcontext, _wordwindow;
  int _wordSize;
  int _wordDim;


  int _inputsize, _token_representation_size;
  int _inputwindow;
  UniLayer<xpu> _olayer_linear;

  //Gated Recursive Unit
  RecursiveGatedNN<xpu> _atom_gatednn;
  MLCRFLoss<xpu> _crf_layer;

  int _atom_composition_layer_num;

  int _labelSize;

  Metric _eval;

  dtype _dropOut;

public:

  inline void init(const NRMat<dtype>& wordEmb, int wordcontext, const NRVec<NRMat<dtype> >& tagEmbs, int labelSize,
      int atom_composition_layer_num) {
    _wordcontext = wordcontext;
    _wordwindow = 2 * _wordcontext + 1;
    _wordSize = wordEmb.nrows();
    _wordDim = wordEmb.ncols();
    // tag variables
    _tagNum = tagEmbs.size();
    _tagSize.resize(_tagNum);
    _tagDim.resize(_tagNum);
    _tags.resize(_tagNum);
    for (int i = 0; i < _tagNum; i++){
      _tagSize[i] = tagEmbs[i].nrows();
      _tagDim[i] = tagEmbs[i].ncols();
      _tags[i].initial(tagEmbs[i]);
    }
    _tag_outputSize = _tagNum * _tagDim[0];

    _token_representation_size = _wordDim + _tag_outputSize;
    _inputsize = _wordwindow * _token_representation_size;

    _labelSize = labelSize;
    _atom_composition_layer_num = atom_composition_layer_num;
    if (atom_composition_layer_num > _wordwindow)
      _atom_composition_layer_num =  _wordwindow;

    _words.initial(wordEmb);

    _inputwindow = _wordwindow;

    for (int idx = 1; idx < _atom_composition_layer_num; idx++) {
      _inputsize += _token_representation_size * (_wordwindow - idx);
      _inputwindow = _inputwindow + (_wordwindow - idx);
    }

    _atom_gatednn.initial(_token_representation_size, 100);

    _olayer_linear.initial(_labelSize, _inputsize, false, 60, 2);
    _crf_layer.initial(_labelSize, 70);

  }

  inline void release() {
    _words.release();
    // add tags release
    for (int i = 0; i < _tagNum; i++){
      _tags[i].release();
    }
    _olayer_linear.release();
    _atom_gatednn.release();
    _crf_layer.release();
  }

  inline dtype process(const vector<Example>& examples, int iter) {
    _eval.reset();   
 
    int example_num = examples.size();
    dtype cost = 0.0;
    int offset = 0;
    int gru_end = _inputwindow;
    int curlayer, curlayerSize, leftchild, rightchild;
    for (int count = 0; count < example_num; count++) {
      const Example& example = examples[count];

      int seq_size = example.m_features.size();

      vector<Tensor<xpu, 2, dtype> > input(seq_size), inputLoss(seq_size);
      vector<Tensor<xpu, 2, dtype> > output(seq_size), outputLoss(seq_size);

      // tag number
      vector<Tensor<xpu, 3, dtype> > tagprime(seq_size), tagprimeLoss(seq_size), tagprimeMask(seq_size);
      vector<Tensor<xpu, 2, dtype> > tagoutput(seq_size), tagoutputLoss(seq_size);
      vector<Tensor<xpu, 2, dtype> > wordprime(seq_size), wordprimeLoss(seq_size), wordprimeMask(seq_size);
      vector<Tensor<xpu, 2, dtype> > wordrepresent(seq_size), wordrepresentLoss(seq_size);

      //GRU
      Tensor<xpu, 2, dtype> inputcontext[seq_size][_inputwindow];
      Tensor<xpu, 2, dtype> inputcontextLoss[seq_size][_inputwindow];

      Tensor<xpu, 2, dtype> inputcontext_reset_left[seq_size][_inputwindow];
      Tensor<xpu, 2, dtype> inputcontext_reset_right[seq_size][_inputwindow];
      Tensor<xpu, 2, dtype> inputcontext_current[seq_size][_inputwindow];

      Tensor<xpu, 2, dtype> inputcontext_gate_left[seq_size][_inputwindow];
      Tensor<xpu, 2, dtype> inputcontext_gate_right[seq_size][_inputwindow];
      Tensor<xpu, 2, dtype> inputcontext_gate_current[seq_size][_inputwindow];
      //end gru

      //initialize
      for (int idx = 0; idx < seq_size; idx++) {
        const Feature& feature = example.m_features[idx];

        for (int idy = 0; idy < _inputwindow; idy++) {
          inputcontext[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
          inputcontextLoss[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);

          inputcontext_current[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
          inputcontext_reset_left[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
          inputcontext_reset_right[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);

          inputcontext_gate_left[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
          inputcontext_gate_right[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
          inputcontext_gate_current[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
        }

        // tag prime init
        tagprime[idx] = NewTensor<xpu>(Shape3(_tagNum, 1, _tagDim[0]), d_zero);
        tagprimeLoss[idx] = NewTensor<xpu>(Shape3(_tagNum, 1, _tagDim[0]), d_zero);
        tagprimeMask[idx] = NewTensor<xpu>(Shape3(_tagNum, 1, _tagDim[0]), d_one);
        tagoutput[idx] = NewTensor<xpu>(Shape2(1, _tag_outputSize), d_zero);
        tagoutputLoss[idx] = NewTensor<xpu>(Shape2(1, _tag_outputSize), d_zero);
        
        wordprime[idx] = NewTensor<xpu>(Shape2(1, _wordDim), d_zero);
        wordprimeLoss[idx] = NewTensor<xpu>(Shape2(1, _wordDim), d_zero);
        wordprimeMask[idx] = NewTensor<xpu>(Shape2(1, _wordDim), d_one);
        wordrepresent[idx] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
        wordrepresentLoss[idx] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
        input[idx] = NewTensor<xpu>(Shape2(1, _inputsize), d_zero);
        inputLoss[idx] = NewTensor<xpu>(Shape2(1, _inputsize), d_zero);
        output[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);
        outputLoss[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);
      }

      //forward propagation
      //input setting, and linear setting
      for (int idx = 0; idx < seq_size; idx++) {
        const Feature& feature = example.m_features[idx];
        //linear features should not be dropped out

        srand(iter * example_num + count * seq_size + idx);

        const vector<int>& words = feature.words;
        _words.GetEmb(words[0], wordprime[idx]);

        dropoutcol(wordprimeMask[idx], _dropOut);
        wordprime[idx] = wordprime[idx] * wordprimeMask[idx];
        
        // tag prime get 
        const vector<int>& tags = feature.tags;
        for (int idy = 0; idy < _tagNum; idy++) {
          _tags[idy].GetEmb(tags[idy], tagprime[idx][idy]);
        }
        // tag drop out
        for (int idy = 0; idy < _tagNum; idy++) {
          dropoutcol(tagprimeMask[idx][idy], _dropOut);
          tagprime[idx][idy] = tagprime[idx][idy] * tagprimeMask[idx][idy];
        }
        concat(tagprime[idx], tagoutput[idx]);
      }
      // concat tag input
      for (int idx = 0; idx < seq_size; idx++) {
        concat(wordprime[idx], tagoutput[idx], wordrepresent[idx]);
      }

      for (int idx = 0; idx < seq_size; idx++) {
        for (int i = -_wordcontext; i <= _wordcontext; i++) {
          if (idx + i >= 0 && idx + i < seq_size)
            inputcontext[idx][i + _wordcontext] += wordrepresent[idx + i];
        }
      }

      for (int idx = 0; idx < seq_size; idx++) {
        //gru
        curlayer = 1;
        curlayerSize = _wordwindow - 1;
        offset = _wordwindow;
        while (curlayer < _atom_composition_layer_num) {
          for (int i = 0; i < curlayerSize; i++) {
            leftchild = offset - curlayerSize - 1;
            rightchild = leftchild + 1;
            _atom_gatednn.ComputeForwardScore(inputcontext[idx][leftchild], inputcontext[idx][rightchild],
                inputcontext_reset_left[idx][offset], inputcontext_reset_right[idx][offset], inputcontext_current[idx][offset],
                inputcontext_gate_left[idx][offset], inputcontext_gate_right[idx][offset], inputcontext_gate_current[idx][offset],
                inputcontext[idx][offset]);
            offset++;
          }
          curlayer++;
          curlayerSize--;
        }
        //end gru
        if (offset != gru_end) {
          std::cout << "error forward computation here" << std::endl;
        }

        //reshape to vector matrix
        offset = 0;
        for (int i = 0; i < _inputwindow; i++) {
          for (int j = 0; j < _token_representation_size; j++) {
            input[idx][0][offset] = inputcontext[idx][i][0][j];
            offset++;
          }
        }

      }

      _olayer_linear.ComputeForwardScore(input, output);

      // get delta for each output
      cost += _crf_layer.loss(output, example.m_labels, outputLoss, _eval, example_num);

      // loss backward propagation
      _olayer_linear.ComputeBackwardLoss(input, output, outputLoss, inputLoss);


      for (int idx = 0; idx < seq_size; idx++) {
        offset = 0;
        for (int i = 0; i < _inputwindow; i++) {
          for (int j = 0; j < _token_representation_size; j++) {
            inputcontextLoss[idx][i][0][j] = inputLoss[idx][0][offset];
            offset++;
          }
        }
        //gru back-propagation
        curlayer = _atom_composition_layer_num - 1;
        curlayerSize = _wordwindow + 1 - _atom_composition_layer_num;
        offset = gru_end - 1;
        while (curlayer > 0) {
          for (int i = curlayerSize - 1; i >= 0; i--) {
            leftchild = offset - curlayerSize - 1;
            rightchild = leftchild + 1;
            //current hidden
            _atom_gatednn.ComputeBackwardLoss(inputcontext[idx][leftchild], inputcontext[idx][rightchild],
                inputcontext_reset_left[idx][offset], inputcontext_reset_right[idx][offset], inputcontext_current[idx][offset],
                inputcontext_gate_left[idx][offset], inputcontext_gate_right[idx][offset], inputcontext_gate_current[idx][offset],
                inputcontext[idx][offset], inputcontextLoss[idx][offset], inputcontextLoss[idx][leftchild], inputcontextLoss[idx][rightchild]);

            offset--;
          }
          curlayer--;
          curlayerSize++;
        }

        //end gru
        if (offset != _wordwindow - 1) {
          std::cout << "error back-propagation here" << std::endl;
        }

      }

      for (int idx = 0; idx < seq_size; idx++) {
        for (int i = -_wordcontext; i <= _wordcontext; i++) {
          if (idx + i >= 0 && idx + i < seq_size)
            wordrepresentLoss[idx + i] += inputcontextLoss[idx][i + _wordcontext];
        }
      }

      // decompose loss with tagoutputLoss
      for (int idx = 0; idx < seq_size; idx++) {
        unconcat(wordprimeLoss[idx], tagoutputLoss[idx], wordrepresentLoss[idx]);
        // tag prime loss
        unconcat(tagprimeLoss[idx], tagoutputLoss[idx]);
      }

      // word fine tune
      if (_words.bEmbFineTune()) {
        for (int idx = 0; idx < seq_size; idx++) {
          const Feature& feature = example.m_features[idx];
          const vector<int>& words = feature.words;
          wordprimeLoss[idx] = wordprimeLoss[idx] * wordprimeMask[idx];
          _words.EmbLoss(words[0], wordprimeLoss[idx]);
        }
      }
      //tag fine tune
      for (int idy = 0; idy < _tagNum; idy++){
        if (_tags[idy].bEmbFineTune()) {
          for (int idx = 0; idx < seq_size; idx++) {
            const Feature& feature = example.m_features[idx];
            const vector<int>& tags = feature.tags;
            tagprimeLoss[idx][idy] = tagprimeLoss[idx][idy] * tagprimeMask[idx][idy];
            _tags[idy].EmbLoss(tags[idy], tagprimeLoss[idx][idy]);
          }
        }
      }

      //release
      for (int idx = 0; idx < seq_size; idx++) {
        for (int idy = 0; idy < _inputwindow; idy++) {
          FreeSpace(&(inputcontext[idx][idy]));
          FreeSpace(&(inputcontextLoss[idx][idy]));

          FreeSpace(&(inputcontext_reset_right[idx][idy]));
          FreeSpace(&(inputcontext_reset_left[idx][idy]));
          FreeSpace(&(inputcontext_current[idx][idy]));

          FreeSpace(&(inputcontext_gate_left[idx][idy]));
          FreeSpace(&(inputcontext_gate_right[idx][idy]));
          FreeSpace(&(inputcontext_gate_current[idx][idy]));
        }

        // tag freespace
        FreeSpace(&(tagprime[idx]));
        FreeSpace(&(tagprimeLoss[idx]));
        FreeSpace(&(tagprimeMask[idx]));
        FreeSpace(&(tagoutput[idx]));
        FreeSpace(&(tagoutputLoss[idx]));

        FreeSpace(&(wordprime[idx]));
        FreeSpace(&(wordprimeLoss[idx]));
        FreeSpace(&(wordprimeMask[idx]));
        FreeSpace(&(wordrepresent[idx]));
        FreeSpace(&(wordrepresentLoss[idx]));

        FreeSpace(&(input[idx]));
        FreeSpace(&(inputLoss[idx]));
        FreeSpace(&(output[idx]));
        FreeSpace(&(outputLoss[idx]));

      }
    }

    if (_eval.getAccuracy() < 0) {
      std::cout << "strange" << std::endl;
    }

    return cost;
  }

  void predict(const vector<Feature>& features, vector<int>& results) {
    int seq_size = features.size();
    int offset = 0;
    int gru_end = _inputwindow;
    int curlayer, curlayerSize, leftchild, rightchild;

    vector<Tensor<xpu, 2, dtype> > input(seq_size);
    vector<Tensor<xpu, 2, dtype> > output(seq_size);

    vector<Tensor<xpu, 3, dtype> > tagprime(seq_size);
    vector<Tensor<xpu, 2, dtype> > tagoutput(seq_size);
    vector<Tensor<xpu, 2, dtype> > wordprime(seq_size);
    vector<Tensor<xpu, 2, dtype> > wordrepresent(seq_size);

    //GRU
    Tensor<xpu, 2, dtype> inputcontext[seq_size][_inputwindow];

    Tensor<xpu, 2, dtype> inputcontext_reset_left[seq_size][_inputwindow];
    Tensor<xpu, 2, dtype> inputcontext_reset_right[seq_size][_inputwindow];
    Tensor<xpu, 2, dtype> inputcontext_current[seq_size][_inputwindow];

    Tensor<xpu, 2, dtype> inputcontext_gate_left[seq_size][_inputwindow];
    Tensor<xpu, 2, dtype> inputcontext_gate_right[seq_size][_inputwindow];
    Tensor<xpu, 2, dtype> inputcontext_gate_current[seq_size][_inputwindow];

    //initialize
    for (int idx = 0; idx < seq_size; idx++) {
      const Feature& feature = features[idx];

      for (int idy = 0; idy < _inputwindow; idy++) {
        inputcontext[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);

        inputcontext_current[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
        inputcontext_reset_left[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
        inputcontext_reset_right[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);

        inputcontext_gate_left[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
        inputcontext_gate_right[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
        inputcontext_gate_current[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
      }

      tagprime[idx] = NewTensor<xpu>(Shape3(_tagNum, 1, _tagDim[0]), d_zero);
      tagoutput[idx] = NewTensor<xpu>(Shape2(1, _tag_outputSize), d_zero);

      wordprime[idx] = NewTensor<xpu>(Shape2(1, _wordDim), d_zero);
      wordrepresent[idx] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);

      input[idx] = NewTensor<xpu>(Shape2(1, _inputsize), d_zero);
      output[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);
    }

    //forward propagation
    //input setting, and linear setting
    for (int idx = 0; idx < seq_size; idx++) {
      const Feature& feature = features[idx];
      //linear features should not be dropped out

      const vector<int>& words = feature.words;
      _words.GetEmb(words[0], wordprime[idx]);

      // tag prime get
      const vector<int>& tags = feature.tags;
      for (int idy = 0; idy < _tagNum; idy++){
        _tags[idy].GetEmb(tags[idy], tagprime[idx][idy]);
      }
      concat(tagprime[idx], tagoutput[idx]);  
    }

    for (int idx = 0; idx < seq_size; idx++) {
      concat(wordprime[idx], tagoutput[idx], wordrepresent[idx]);
    }

    for (int idx = 0; idx < seq_size; idx++) {
      for (int i = -_wordcontext; i <= _wordcontext; i++) {
        if (idx + i >= 0 && idx + i < seq_size)
          inputcontext[idx][i + _wordcontext] += wordrepresent[idx + i];
      }
    }

    for (int idx = 0; idx < seq_size; idx++) {
      //gru
      curlayer = 1;
      curlayerSize = _wordwindow - 1;
      offset = _wordwindow;
      while (curlayer < _atom_composition_layer_num) {
        for (int i = 0; i < curlayerSize; i++) {
          leftchild = offset - curlayerSize - 1;
          rightchild = leftchild + 1;
          _atom_gatednn.ComputeForwardScore(inputcontext[idx][leftchild], inputcontext[idx][rightchild],
              inputcontext_reset_left[idx][offset], inputcontext_reset_right[idx][offset], inputcontext_current[idx][offset],
              inputcontext_gate_left[idx][offset], inputcontext_gate_right[idx][offset], inputcontext_gate_current[idx][offset],
              inputcontext[idx][offset]);
          offset++;
        }
        curlayer++;
        curlayerSize--;
      }
      //end gru
      if (offset != gru_end) {
        std::cout << "error forward computation here" << std::endl;
      }

      //reshape to vector matrix
      offset = 0;
      for (int i = 0; i < _inputwindow; i++) {
        for (int j = 0; j < _token_representation_size; j++) {
          input[idx][0][offset] = inputcontext[idx][i][0][j];
          offset++;
        }
      }

    }

    _olayer_linear.ComputeForwardScore(input, output);

    // decode algorithm
    _crf_layer.predict(output, results);

    //release
    for (int idx = 0; idx < seq_size; idx++) {
      for (int idy = 0; idy < _inputwindow; idy++) {
        FreeSpace(&(inputcontext[idx][idy]));

        FreeSpace(&(inputcontext_reset_right[idx][idy]));
        FreeSpace(&(inputcontext_reset_left[idx][idy]));
        FreeSpace(&(inputcontext_current[idx][idy]));

        FreeSpace(&(inputcontext_gate_left[idx][idy]));
        FreeSpace(&(inputcontext_gate_right[idx][idy]));
        FreeSpace(&(inputcontext_gate_current[idx][idy]));
      }
      FreeSpace(&(tagprime[idx]));
      FreeSpace(&(tagoutput[idx]));
      FreeSpace(&(wordprime[idx]));
      FreeSpace(&(wordrepresent[idx]));
      FreeSpace(&(input[idx]));
      FreeSpace(&(output[idx]));
    }
  }

  dtype computeScore(const Example& example) {
    int seq_size = example.m_features.size();
    int offset = 0;
    int gru_end = _inputwindow;
    int curlayer, curlayerSize, leftchild, rightchild;

    vector<Tensor<xpu, 2, dtype> > input(seq_size);
    vector<Tensor<xpu, 2, dtype> > output(seq_size);

    vector<Tensor<xpu, 3, dtype> > tagprime(seq_size);
    vector<Tensor<xpu, 2, dtype> > tagoutput(seq_size);
    vector<Tensor<xpu, 2, dtype> > wordprime(seq_size);
    vector<Tensor<xpu, 2, dtype> > wordrepresent(seq_size);

    //GRU
    Tensor<xpu, 2, dtype> inputcontext[seq_size][_inputwindow];

    Tensor<xpu, 2, dtype> inputcontext_reset_left[seq_size][_inputwindow];
    Tensor<xpu, 2, dtype> inputcontext_reset_right[seq_size][_inputwindow];
    Tensor<xpu, 2, dtype> inputcontext_current[seq_size][_inputwindow];

    Tensor<xpu, 2, dtype> inputcontext_gate_left[seq_size][_inputwindow];
    Tensor<xpu, 2, dtype> inputcontext_gate_right[seq_size][_inputwindow];
    Tensor<xpu, 2, dtype> inputcontext_gate_current[seq_size][_inputwindow];


    //initialize
    for (int idx = 0; idx < seq_size; idx++) {
      const Feature& feature = example.m_features[idx];

      for (int idy = 0; idy < _inputwindow; idy++) {
        inputcontext[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);

        inputcontext_current[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
        inputcontext_reset_left[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
        inputcontext_reset_right[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);

        inputcontext_gate_left[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
        inputcontext_gate_right[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
        inputcontext_gate_current[idx][idy] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);
      }

      tagprime[idx] = NewTensor<xpu>(Shape3(_tagNum, 1, _tagDim[0]), d_zero);
      tagoutput[idx] = NewTensor<xpu>(Shape2(1, _tag_outputSize), d_zero);

      wordprime[idx] = NewTensor<xpu>(Shape2(1, _wordDim), d_zero);
      wordrepresent[idx] = NewTensor<xpu>(Shape2(1, _token_representation_size), d_zero);

      input[idx] = NewTensor<xpu>(Shape2(1, _inputsize), d_zero);
      output[idx] = NewTensor<xpu>(Shape2(1, _labelSize), d_zero);
    }

    //forward propagation
    //input setting, and linear setting
    for (int idx = 0; idx < seq_size; idx++) {
      const Feature& feature = example.m_features[idx];
      //linear features should not be dropped out

      const vector<int>& words = feature.words;
      _words.GetEmb(words[0], wordprime[idx]);

      // tag prime get
      const vector<int>& tags = feature.tags;
      for (int idy = 0; idy < _tagNum; idy++){
        _tags[idy].GetEmb(tags[idy], tagprime[idx][idy]);
      }
      concat(tagprime[idx], tagoutput[idx]);  
    }

    for (int idx = 0; idx < seq_size; idx++) {
      concat(wordprime[idx], tagoutput[idx], wordrepresent[idx]);
    }

    for (int idx = 0; idx < seq_size; idx++) {
      for (int i = -_wordcontext; i <= _wordcontext; i++) {
        if (idx + i >= 0 && idx + i < seq_size)
          inputcontext[idx][i + _wordcontext] += wordrepresent[idx + i];
      }
    }

    for (int idx = 0; idx < seq_size; idx++) {
      //gru
      curlayer = 1;
      curlayerSize = _wordwindow - 1;
      offset = _wordwindow;
      while (curlayer < _atom_composition_layer_num) {
        for (int i = 0; i < curlayerSize; i++) {
          leftchild = offset - curlayerSize - 1;
          rightchild = leftchild + 1;
          _atom_gatednn.ComputeForwardScore(inputcontext[idx][leftchild], inputcontext[idx][rightchild],
              inputcontext_reset_left[idx][offset], inputcontext_reset_right[idx][offset], inputcontext_current[idx][offset],
              inputcontext_gate_left[idx][offset], inputcontext_gate_right[idx][offset], inputcontext_gate_current[idx][offset],
              inputcontext[idx][offset]);
          offset++;
        }
        curlayer++;
        curlayerSize--;
      }
      //end gru
      if (offset != gru_end) {
        std::cout << "error forward computation here" << std::endl;
      }

      //reshape to vector matrix
      offset = 0;
      for (int i = 0; i < _inputwindow; i++) {
        for (int j = 0; j < _token_representation_size; j++) {
          input[idx][0][offset] = inputcontext[idx][i][0][j];
          offset++;
        }
      }

    }

    _olayer_linear.ComputeForwardScore(input, output);

    // get delta for each output
    dtype cost = _crf_layer.cost(output, example.m_labels);

    //release
    for (int idx = 0; idx < seq_size; idx++) {
      for (int idy = 0; idy < _inputwindow; idy++) {
        FreeSpace(&(inputcontext[idx][idy]));

        FreeSpace(&(inputcontext_reset_right[idx][idy]));
        FreeSpace(&(inputcontext_reset_left[idx][idy]));
        FreeSpace(&(inputcontext_current[idx][idy]));

        FreeSpace(&(inputcontext_gate_left[idx][idy]));
        FreeSpace(&(inputcontext_gate_right[idx][idy]));
        FreeSpace(&(inputcontext_gate_current[idx][idy]));
      }
      FreeSpace(&(tagprime[idx]));
      FreeSpace(&(tagoutput[idx]));
      FreeSpace(&(wordprime[idx]));
      FreeSpace(&(wordrepresent[idx]));
      FreeSpace(&(input[idx]));
      FreeSpace(&(output[idx]));
    }
    return cost;
  }

  void updateParams(dtype nnRegular, dtype adaAlpha, dtype adaEps) {
    _olayer_linear.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    _atom_gatednn.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    _crf_layer.updateAdaGrad(nnRegular, adaAlpha, adaEps);

    _words.updateAdaGrad(nnRegular, adaAlpha, adaEps);
    for (int i = 0; i < _tagNum; i++){
      _tags[i].updateAdaGrad(nnRegular, adaAlpha, adaEps);
    }
    
  }

  void writeModel();

  void loadModel();

  void checkgrad(const vector<Example>& examples, Tensor<xpu, 2, dtype> Wd, Tensor<xpu, 2, dtype> gradWd, const string& mark, int iter) {
    int charseed = mark.length();
    for (int i = 0; i < mark.length(); i++) {
      charseed = (int) (mark[i]) * 5 + charseed;
    }
    srand(iter + charseed);
    std::vector<int> idRows, idCols;
    idRows.clear();
    idCols.clear();
    for (int i = 0; i < Wd.size(0); ++i)
      idRows.push_back(i);
    for (int idx = 0; idx < Wd.size(1); idx++)
      idCols.push_back(idx);

    random_shuffle(idRows.begin(), idRows.end());
    random_shuffle(idCols.begin(), idCols.end());

    int check_i = idRows[0], check_j = idCols[0];

    dtype orginValue = Wd[check_i][check_j];

    Wd[check_i][check_j] = orginValue + 0.001;
    dtype lossAdd = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossAdd += computeScore(oneExam);
    }

    Wd[check_i][check_j] = orginValue - 0.001;
    dtype lossPlus = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossPlus += computeScore(oneExam);
    }

    dtype mockGrad = (lossAdd - lossPlus) / 0.002;
    mockGrad = mockGrad / examples.size();
    dtype computeGrad = gradWd[check_i][check_j];

    printf("Iteration %d, Checking gradient for %s[%d][%d]:\t", iter, mark.c_str(), check_i, check_j);
    printf("mock grad = %.18f, computed grad = %.18f\n", mockGrad, computeGrad);

    Wd[check_i][check_j] = orginValue;
  }

  void checkgrad(const vector<Example>& examples, Tensor<xpu, 2, dtype> Wd, Tensor<xpu, 2, dtype> gradWd, const string& mark, int iter,
      const hash_set<int>& indexes, bool bRow = true) {
    int charseed = mark.length();
    for (int i = 0; i < mark.length(); i++) {
      charseed = (int) (mark[i]) * 5 + charseed;
    }
    srand(iter + charseed);
    std::vector<int> idRows, idCols;
    idRows.clear();
    idCols.clear();
    static hash_set<int>::iterator it;
    if (bRow) {
      for (it = indexes.begin(); it != indexes.end(); ++it)
        idRows.push_back(*it);
      for (int idx = 0; idx < Wd.size(1); idx++)
        idCols.push_back(idx);
    } else {
      for (it = indexes.begin(); it != indexes.end(); ++it)
        idCols.push_back(*it);
      for (int idx = 0; idx < Wd.size(0); idx++)
        idRows.push_back(idx);
    }

    random_shuffle(idRows.begin(), idRows.end());
    random_shuffle(idCols.begin(), idCols.end());

    int check_i = idRows[0], check_j = idCols[0];

    dtype orginValue = Wd[check_i][check_j];

    Wd[check_i][check_j] = orginValue + 0.001;
    dtype lossAdd = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossAdd += computeScore(oneExam);
    }

    Wd[check_i][check_j] = orginValue - 0.001;
    dtype lossPlus = 0.0;
    for (int i = 0; i < examples.size(); i++) {
      Example oneExam = examples[i];
      lossPlus += computeScore(oneExam);
    }

    dtype mockGrad = (lossAdd - lossPlus) / 0.002;
    mockGrad = mockGrad / examples.size();
    dtype computeGrad = gradWd[check_i][check_j];

    printf("Iteration %d, Checking gradient for %s[%d][%d]:\t", iter, mark.c_str(), check_i, check_j);
    printf("mock grad = %.18f, computed grad = %.18f\n", mockGrad, computeGrad);

    Wd[check_i][check_j] = orginValue;

  }

  void checkgrads(const vector<Example>& examples, int iter) {
    checkgrad(examples, _olayer_linear._W, _olayer_linear._gradW, "_olayer_linear._W", iter);



    checkgrad(examples, _atom_gatednn._reset_left._W, _atom_gatednn._reset_left._gradW, "_atom_gatednn._reset_left._W", iter);
    checkgrad(examples, _atom_gatednn._reset_right._W, _atom_gatednn._reset_right._gradW, "_atom_gatednn._reset_right._W", iter);
    checkgrad(examples, _atom_gatednn._update_left._W, _atom_gatednn._update_left._gradW, "_atom_gatednn._update_left._W", iter);
    checkgrad(examples, _atom_gatednn._update_right._W, _atom_gatednn._update_right._gradW, "_atom_gatednn._update_right._W", iter);
    checkgrad(examples, _atom_gatednn._update_tilde._W, _atom_gatednn._update_tilde._gradW, "_atom_gatednn._update_tilde._W", iter);
    checkgrad(examples, _atom_gatednn._recursive_tilde._WL, _atom_gatednn._recursive_tilde._gradWL, "_atom_gatednn._recursive_tilde._WL", iter);
    checkgrad(examples, _atom_gatednn._recursive_tilde._WR, _atom_gatednn._recursive_tilde._gradWR, "_atom_gatednn._recursive_tilde._WR", iter);
    checkgrad(examples, _atom_gatednn._recursive_tilde._b, _atom_gatednn._recursive_tilde._gradb, "_atom_gatednn._recursive_tilde._b", iter);

    checkgrad(examples, _crf_layer._tagBigram, _crf_layer._grad_tagBigram, "_crf_layer._tagBigram", iter);

    checkgrad(examples, _words._E, _words._gradE, "_words._E", iter, _words._indexers);
    // tag checkgrad
    for (int i = 0; i < _tagNum; i++){
      checkgrad(examples, _tags[i]._E, _tags[i]._gradE, "_tags._E", iter, _tags[i]._indexers);
    }

  }

public:
  inline void resetEval() {
    _eval.reset();
  }

  inline void setDropValue(dtype dropOut) {
    _dropOut = dropOut;
  }

  inline void setWordEmbFinetune(bool b_wordEmb_finetune) {
    _words.setEmbFineTune(b_wordEmb_finetune);
  }
  
  inline void setTagEmbFinetune(bool b_tagEmb_finetune) {
    for (int idx = 0; idx < _tagNum; idx++){
      _tags[idx].setEmbFineTune(b_tagEmb_finetune);
    }   
  }


};

#endif /* SRC_GatedCRFMLClassifier_H_ */