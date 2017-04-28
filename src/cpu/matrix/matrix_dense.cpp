#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <numeric>
//#include <execution>

#include "gsl/gsl_blas.h"
#include "gsl/gsl_matrix.h"
#include "gsl/gsl_vector.h"
#include "equil_helper.h"
#include "matrix/matrix.h"
#include "matrix/matrix_dense.h"
#include "util.h"




namespace pogs {

////////////////////////////////////////////////////////////////////////////////
////////////////////////////// Helper Functions ////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
namespace {

// File scoped constants.
const NormTypes kNormEquilibrate = kNorm2; 
const NormTypes kNormNormalize   = kNormFro;

template<typename T>
struct CpuData {
  const T *orig_data;
  CpuData(const T *orig_data) : orig_data(orig_data) { }
};

CBLAS_TRANSPOSE_t OpToCblasOp(char trans) {
  ASSERT(trans == 'n' || trans == 'N' || trans == 't' || trans == 'T');
  return trans == 'n' || trans == 'N' ? CblasNoTrans : CblasTrans;
}

template <typename T>
T NormEst(NormTypes norm_type, const MatrixDense<T>& A);

template <typename T>
void MultDiag(const T *d, const T *e, size_t m, size_t n,
              typename MatrixDense<T>::Ord ord, T *data);

}  // namespace

  // TODO: Can have Equil (or whatever wants to modify _data) called by single core first when inporting data into _data.  Then rest of cores (in multi-threaded call to another MatrixDense(wDev,A) would not do equil and could pass only the pointer instead of creating new memory.
  // TODO: For first core, could assign pointer if ok with input being modified.  Could have function call argument that allows user to say if ok to modify input data in order to save memory.
  
////////////////////////////////////////////////////////////////////////////////
/////////////////////// MatrixDense Implementation /////////////////////////////
////////////////////////////////////////////////////////////////////////////////
template <typename T>
MatrixDense<T>::MatrixDense(int sharedA, int wDev, char ord, size_t m, size_t n, const T *data)
  : Matrix<T>(m, n, 0), _sharedA(sharedA), _wDev(wDev), _datatype(0), _data(0), _de(0) {
  _me=_wDev; // assume thread=wDev if not given  
  _datay=NULL;
  _vdata=NULL;
  _vdatay=NULL;

  ASSERT(ord == 'r' || ord == 'R' || ord == 'c' || ord == 'C');
  _ord = (ord == 'r' || ord == 'R') ? ROW : COL;

  // Set GPU specific _info.
  CpuData<T> *info = new CpuData<T>(data);
  this->_info = reinterpret_cast<void*>(info);
  CpuData<T> *infoy = new CpuData<T>(0); // new structure (holds pointer to data and GPU handle)
  this->_infoy = reinterpret_cast<void*>(infoy);
  CpuData<T> *vinfo = new CpuData<T>(0); // new structure (holds pointer to data and GPU handle)
  this->_vinfo = reinterpret_cast<void*>(vinfo);
  CpuData<T> *vinfoy = new CpuData<T>(0); // new structure (holds pointer to data and GPU handle)
  this->_vinfoy = reinterpret_cast<void*>(vinfoy);
  
  // Copy Matrix to CPU
  if(!this->_done_alloc){
    this->_done_alloc = true;
    if(sharedA!=0){ // can't because _data contents get modified, unless sharing and equilibrating here
      _data = const_cast<T*>(data);
    }
    else{
      _data = new T[this->_m * this->_n]; ASSERT(_data != 0);  memcpy(_data, info->orig_data, this->_m * this->_n * sizeof(T));
    }
    _de = new T[this->_m + this->_n]; ASSERT(_de != 0);memset(_de, 0, (this->_m + this->_n) * sizeof(T));
    if(sharedA>0){
      Init();
      Equil(1); // JONTODO: hack for now, should pass user bool
    }
  }

}

template <typename T>
MatrixDense<T>::MatrixDense(char ord, size_t m, size_t n, const T *data)
  : MatrixDense<T>(0,0,ord,m,n,data){} // assume sharedA=0 and wDev=0



  // no use of datatype when on CPU
template <typename T>
MatrixDense<T>::MatrixDense(int sharedA, int wDev, int datatype, char ord, size_t m, size_t n, T *data)
  : Matrix<T>(m, n, 0), _sharedA(sharedA), _wDev(wDev), _datatype(datatype),_data(0), _de(0) {
  _me=_wDev; // assume thread=wDev if not given
  _datay=NULL;
  _vdata=NULL;
  _vdatay=NULL;

  ASSERT(ord == 'r' || ord == 'R' || ord == 'c' || ord == 'C');
  _ord = (ord == 'r' || ord == 'R') ? ROW : COL;

  // Set GPU specific _info.
  CpuData<T> *info = new CpuData<T>(data);
  this->_info = reinterpret_cast<void*>(info);
  CpuData<T> *infoy = new CpuData<T>(0); // new structure (holds pointer to data and GPU handle)
  this->_infoy = reinterpret_cast<void*>(infoy);
  CpuData<T> *vinfo = new CpuData<T>(0); // new structure (holds pointer to data and GPU handle)
  this->_vinfo = reinterpret_cast<void*>(vinfo);
  CpuData<T> *vinfoy = new CpuData<T>(0); // new structure (holds pointer to data and GPU handle)
  this->_vinfoy = reinterpret_cast<void*>(vinfoy);

  if(!this->_done_alloc){
    this->_done_alloc = true;
    if(sharedA!=0){ // can't in case original pointer came from one thread
      _data = data;
    }
    else{
      _data = new T[this->_m * this->_n];
      ASSERT(_data != 0);
      memcpy(_data, info->orig_data, this->_m * this->_n * sizeof(T));
    }
    _de = new T[this->_m + this->_n]; ASSERT(_de != 0);memset(_de, 0, (this->_m + this->_n) * sizeof(T));
    if(sharedA>0){
      Init();
      Equil(1); // JONTODO: hack for now, should pass user bool
    }
  }


}
template <typename T>
MatrixDense<T>::MatrixDense(int sharedA, int me, int wDev, char ord, size_t m, size_t n, size_t mValid, const T *data, const T *datay, const T *vdata, const T *vdatay)
  : Matrix<T>(m, n, mValid), _sharedA(sharedA), _me(me), _wDev(wDev), _datatype(0),_data(0), _datay(0), _vdata(0), _vdatay(0), _de(0) {

  ASSERT(ord == 'r' || ord == 'R' || ord == 'c' || ord == 'C');
  _ord = (ord == 'r' || ord == 'R') ? ROW : COL;
  DEBUG_FPRINTF(stderr,"ord=%c m=%d n=%d mValid=%d\n",ord,(int)m,(int)n,int(mValid));

  CpuData<T> *info = new CpuData<T>(data); // new structure (holds pointer to data and GPU handle)
  CpuData<T> *infoy = new CpuData<T>(datay); // new structure (holds pointer to data and GPU handle)
  CpuData<T> *vinfo = new CpuData<T>(vdata); // new structure (holds pointer to data and GPU handle)
  CpuData<T> *vinfoy = new CpuData<T>(vdatay); // new structure (holds pointer to data and GPU handle)
  this->_info = reinterpret_cast<void*>(info);
  this->_infoy = reinterpret_cast<void*>(infoy);
  this->_vinfo = reinterpret_cast<void*>(vinfo);
  this->_vinfoy = reinterpret_cast<void*>(vinfoy);


  if(!this->_done_alloc){
    this->_done_alloc = true;
    if(sharedA!=0){ // can't because _data contents get modified, unless do sharedA case and Equil is processed locally before given to other threads
      _data = const_cast<T*>(data);
      _datay = const_cast<T*>(datay);
      _vdata = const_cast<T*>(vdata);
      _vdatay = const_cast<T*>(vdatay);
    }
    else{
      // TODO: Properly free these at end if allocated.  Just need flag to say if allocated, as can't just check if NULL or not.
      fprintf(stderr,"this->_m=%d this->_n=%d sizeof(T)=%d\n",this->_m,this->_n,sizeof(T)); fflush(stderr);
      if(info->orig_data){
        _data = new T[this->_m * this->_n];
        ASSERT(_data != 0);
        memcpy(_data, info->orig_data, this->_m * this->_n * sizeof(T));
      }

      if(infoy->orig_data){
        _datay = new T[this->_m];
        ASSERT(_datay != 0);
        memcpy(_datay, infoy->orig_data, this->_m * sizeof(T));
      }

      if(vinfo->orig_data){
        _vdata = new T[this->_mvalid * this->_n];
        ASSERT(_vdata != 0);
        memcpy(_vdata, vinfo->orig_data, this->_mvalid * this->_n * sizeof(T));
      }

      if(vinfoy->orig_data){
        _vdatay = new T[this->_mvalid];
        ASSERT(_vdatay != 0);
        memcpy(_vdatay, vinfoy->orig_data, this->_mvalid * sizeof(T));
      }
    }
    _de = new T[this->_m + this->_n]; ASSERT(_de != 0);memset(_de, 0, (this->_m + this->_n) * sizeof(T)); // not needed in existing code when sharedA<0
    if(sharedA>0){
      Init();
      Equil(1); // JONTODO: hack for now, should pass user bool
    }
  }
}



template <typename T>
MatrixDense<T>::MatrixDense(int wDev, char ord, size_t m, size_t n, size_t mValid, const T *data, const T *datay, const T *vdata, const T *vdatay)
  : MatrixDense<T>(0,wDev,wDev,ord,m,n,mValid,data,datay,vdata,vdatay){} // assume sharedA=0 and source thread=wDev always if not given
 

  // no use of datatype for CPU version
  // Assume call this function when oustide parallel region and first instance of call to allocating matrix A (_data), etc.
template <typename T>
MatrixDense<T>::MatrixDense(int sharedA, int me, int wDev, int datatype, char ord, size_t m, size_t n, size_t mValid, T *data, T *datay, T *vdata, T *vdatay)
  : Matrix<T>(m, n, mValid), _sharedA(sharedA), _me(me), _wDev(wDev), _datatype(datatype),_data(0), _datay(0), _vdata(0), _vdatay(0), _de(0) {

  _ord = (ord == 'r' || ord == 'R') ? ROW : COL;

  CpuData<T> *info = new CpuData<T>(data);
  CpuData<T> *infoy = new CpuData<T>(datay);
  CpuData<T> *vinfo = new CpuData<T>(vdata);
  CpuData<T> *vinfoy = new CpuData<T>(vdatay);
  this->_info = reinterpret_cast<void*>(info);
  this->_infoy = reinterpret_cast<void*>(infoy);
  this->_vinfo = reinterpret_cast<void*>(vinfo);
  this->_vinfoy = reinterpret_cast<void*>(vinfoy);

  if(!this->_done_alloc){
    this->_done_alloc = true;
    if(sharedA!=0){ // can't do this in case input pointers were from one thread and this function called on multiple threads.  However, currently, this function is only called by outside parallel region when getting first copying. For sharedA case, minimize memory use overall, so allow pointer assignment here just for source (this assumes scoring using internal calculation, not using OLDPREDS because matrix is modified).  So this call isn't only because shared memory case, but rather want minimal memory even on source thread in shared memory case
      _data = const_cast<T*>(data);
      _datay = const_cast<T*>(datay);
      _vdata = const_cast<T*>(vdata);
      _vdatay = const_cast<T*>(vdatay);
    }
    else{
      // TODO: Properly free these at end if allocated.  Just need flag to say if allocated, as can't just check if NULL or not.

      if(info->orig_data){
        _data = new T[this->_m * this->_n];
        ASSERT(_data != 0);
        memcpy(_data, info->orig_data, this->_m * this->_n * sizeof(T));
      }

      if(infoy->orig_data){
        _datay = new T[this->_m];
        ASSERT(_datay != 0);
        memcpy(_datay, infoy->orig_data, this->_m * sizeof(T));
      }

      if(vinfo->orig_data){
        _vdata = new T[this->_mvalid * this->_n];
        ASSERT(_vdata != 0);
        memcpy(_vdata, vinfo->orig_data, this->_mvalid * this->_n * sizeof(T));
      }

      if(vinfoy->orig_data){
        _vdatay = new T[this->_mvalid];
        ASSERT(_vdatay != 0);
        memcpy(_vdatay, vinfoy->orig_data, this->_mvalid * sizeof(T));
      }
    }
    _de = new T[this->_m + this->_n]; ASSERT(_de != 0);memset(_de, 0, (this->_m + this->_n) * sizeof(T)); // NOTE: If passing pointers, only pass data pointers out and back in in this function, so _de still needs to get allocated and equlilibrated.  This means allocation and equilibration done twice effectively.  Can avoid during first pointer assignment if want to pass user option JONTODO
    if(sharedA>0){
      Init();
      Equil(1); // JONTODO: hack for now, should pass user bool
    }
  }

  
#if 0
    if (ord=='r') {
      std::cout << m << std::endl;
      std::cout << n << std::endl;
      for (int i=0; i<m; ++i) {
        std::cout << std::endl;
        for (int j = 0; j < n; ++j) {
          std::cout<< *(_data + i*n+j) << " ";
        }
        std::cout << " -> " << *(_datay + i);
      }
    } else {
      std::cout << m << std::endl;
      std::cout << n << std::endl;
      for (int i=0; i<m; ++i) {
        std::cout << std::endl;
        for (int j = 0; j < n; ++j) {
          std::cout<< *(_data + j*m+i) << " ";
        }
        std::cout << " -> " << *(_datay + i);
      }
    }
#endif

}



template <typename T>
MatrixDense<T>::MatrixDense(int wDev, int datatype, char ord, size_t m, size_t n, size_t mValid, T *data, T *datay, T *vdata, T *vdatay)
  : MatrixDense<T>(0,wDev,wDev,datatype,ord,m,n,mValid,data,datay,vdata,vdatay){} // assume sharedA=0 and thread=wDev if not given

  
  
template <typename T>
MatrixDense<T>::MatrixDense(int sharedA, int me, int wDev, const MatrixDense<T>& A)
  : Matrix<T>(A._m, A._n, A._mvalid), _sharedA(sharedA), _me(me), _wDev(wDev), _data(0), _datay(0), _vdata(0), _vdatay(0), _de(0), _ord(A._ord) {

  CpuData<T> *info_A   = reinterpret_cast<CpuData<T>*>(A._info); // cast from void to CpuData
  CpuData<T> *infoy_A  = reinterpret_cast<CpuData<T>*>(A._infoy); // cast from void to CpuData
  CpuData<T> *vinfo_A  = reinterpret_cast<CpuData<T>*>(A._vinfo); // cast from void to CpuData
  CpuData<T> *vinfoy_A = reinterpret_cast<CpuData<T>*>(A._vinfoy); // cast from void to CpuData

  CpuData<T> *info;
  CpuData<T> *infoy;
  CpuData<T> *vinfo;
  CpuData<T> *vinfoy;
  if(A._data) info = new CpuData<T>(info_A->orig_data); // create new CpuData structure with point to CPU data
  else info = new CpuData<T>(0);
  if(A._datay) infoy  = new CpuData<T>(infoy_A->orig_data); // create new CpuData structure with point to CPU data
  else info = new CpuData<T>(0);
  if(A._vdata) vinfo  = new CpuData<T>(vinfo_A->orig_data); // create new CpuData structure with point to CPU data
  else info = new CpuData<T>(0);
  if(A._vdatay) vinfoy = new CpuData<T>(vinfoy_A->orig_data); // create new CpuData structure with point to CPU data
  else info = new CpuData<T>(0);

  if(A._data) this->_info = reinterpret_cast<void*>(info); // back to cast as void
  if(A._datay) this->_infoy = reinterpret_cast<void*>(infoy); // back to cast as void
  if(A._vdata)  this->_vinfo = reinterpret_cast<void*>(vinfo); // back to cast as void
  if(A._vdatay) this->_vinfoy = reinterpret_cast<void*>(vinfoy); // back to cast as void          

  if(!this->_done_alloc){
    this->_done_alloc = true;

    // remove A._wDev==wDev && here compared to GPU version.  Could apply if wDev was used and meant something about MPI node id, but not implemented
    if(A._me == _me || sharedA!=0){ // otherwise, can't do this because original CPU call to MatrixDense(wDev,...data) allocates _data outside openmp scope, and then this function will be called per thread and each thread needs its own _data in order to handle moidfying matrix A with d and e.  But if sharedA=1, then expect source thread to have already modified matrix, so can just do pointer assignment.
      _data   = A._data;
      _datay  = A._datay;
      _vdata  = A._vdata;
      _vdatay = A._vdatay;
      _de = A._de; // now share de as never gets modified after original A was processed
      //      Init();
      //      this->_done_equil=1;
    }
    else{ // then allocate duplicate _data, etc. for this thread
      if(A._data){
        _data = new T[A._m * A._n];
        ASSERT(_data != 0);
        memcpy(_data, info_A->orig_data, A._m * A._n * sizeof(T)); 
      }

      if(A._datay){
        _datay = new T[A._m];
        ASSERT(_datay != 0);
        memcpy(_datay, infoy_A->orig_data, A._m * sizeof(T));
      }

      if(A._vdata){
        _vdata = new T[A._mvalid * A._n];
        ASSERT(_vdata != 0);
        memcpy(_vdata, vinfo_A->orig_data, A._mvalid * A._n * sizeof(T));
      }
    
      if(A._vdatay){
        _vdatay = new T[A._mvalid];
        ASSERT(_vdatay != 0);
        memcpy(_vdatay, vinfoy_A->orig_data, A._mvalid * sizeof(T));
      }
      _de = new T[this->_m + this->_n]; ASSERT(_de != 0);memset(_de, 0, (this->_m + this->_n) * sizeof(T));
      if(sharedA>0){
        Init();
        Equil(1); // JONTODO: hack for now, should pass user bool
      }
    }
  }
  
}


template <typename T>
MatrixDense<T>::MatrixDense(int me, int wDev, const MatrixDense<T>& A)
  : MatrixDense<T>(0, wDev, wDev, A){} // then assume no shared memory by default

  template <typename T>
MatrixDense<T>::MatrixDense(int wDev, const MatrixDense<T>& A)
  : MatrixDense<T>(wDev, wDev, A){} // then assume thread=wDev if not given

template <typename T>
MatrixDense<T>::MatrixDense(const MatrixDense<T>& A)
  : MatrixDense<T>(A._wDev, A){}



  template <typename T>
MatrixDense<T>::~MatrixDense() {
  CpuData<T> *info = reinterpret_cast<CpuData<T>*>(this->_info);
  CpuData<T> *infoy = reinterpret_cast<CpuData<T>*>(this->_infoy);
  CpuData<T> *vinfo = reinterpret_cast<CpuData<T>*>(this->_vinfo);
  CpuData<T> *vinfoy = reinterpret_cast<CpuData<T>*>(this->_vinfoy);
  if(info) delete info;
  if(infoy) delete infoy;
  if(vinfo) delete vinfo;
  if(vinfoy) delete vinfoy;
  this->_info = 0;
  this->_infoy = 0;
  this->_vinfo = 0;
  this->_vinfoy = 0;

  // JONTODO: why aren't _data etc. freed?
}

template <typename T>
int MatrixDense<T>::Init() {
  DEBUG_EXPECT(!this->_done_init);
  if (this->_done_init)
    return 1;
  this->_done_init = true;

  return 0;
}

template <typename T>
void MatrixDense<T>::GetTrainX(int datatype, size_t size, T**data) const {
  std::memcpy(*data, _data, size * sizeof(T));
}
template <typename T>
void MatrixDense<T>::GetTrainY(int datatype, size_t size, T**data) const {
  std::memcpy(*data, _datay, size * sizeof(T));
}

template <typename T>
void MatrixDense<T>::GetValidX(int datatype, size_t size, T**data) const {
  std::memcpy(*data, _vdata, size * sizeof(T));
}
template <typename T>
void MatrixDense<T>::GetValidY(int datatype, size_t size, T**data) const {
  std::memcpy(*data, _vdatay, size * sizeof(T));
}


  template <typename T>
int MatrixDense<T>::Mul(char trans, T alpha, const T *x, T beta, T *y) const {
DEBUG_EXPECT(this->_done_init);
if (!this->_done_init)
  return 1;

const gsl::vector<T> x_vec = gsl::vector_view_array<T>(x, this->_n);
gsl::vector<T> y_vec = gsl::vector_view_array<T>(y, this->_m);

if (_ord == ROW) {
  gsl::matrix<T, CblasRowMajor> A =
      gsl::matrix_view_array<T, CblasRowMajor>(_data, this->_m, this->_n);
  gsl::blas_gemv(OpToCblasOp(trans), alpha, &A, &x_vec, beta,
      &y_vec);
  } else {
    gsl::matrix<T, CblasColMajor> A =
        gsl::matrix_view_array<T, CblasColMajor>(_data, this->_m, this->_n);
    gsl::blas_gemv(OpToCblasOp(trans), alpha, &A, &x_vec, beta, &y_vec);
  }

  return 0;
}

  template <typename T>
int MatrixDense<T>::Mulvalid(char trans, T alpha, const T *x, T beta, T *y) const {
DEBUG_EXPECT(this->_done_init);
if (!this->_done_init)
  return 1;

const gsl::vector<T> x_vec = gsl::vector_view_array<T>(x, this->_n);
gsl::vector<T> y_vec = gsl::vector_view_array<T>(y, this->_mvalid);

if (_ord == ROW) {
  gsl::matrix<T, CblasRowMajor> A =
      gsl::matrix_view_array<T, CblasRowMajor>(_vdata, this->_mvalid, this->_n);
  gsl::blas_gemv(OpToCblasOp(trans), alpha, &A, &x_vec, beta,
      &y_vec);
  } else {
    gsl::matrix<T, CblasColMajor> A =
        gsl::matrix_view_array<T, CblasColMajor>(_vdata, this->_mvalid, this->_n);
    gsl::blas_gemv(OpToCblasOp(trans), alpha, &A, &x_vec, beta, &y_vec);
  }

  return 0;
}

template <typename T>
int MatrixDense<T>::Equil(bool equillocal) {
  //  fprintf(stderr,"In Equil: done_init=%d done_equil=%d\n",this->_done_init,this->_done_equil); fflush(stderr);
  DEBUG_ASSERT(this->_done_init);
  if (!this->_done_init)
    return 1;
  
  if (this->_done_equil) return 0;
  else this->_done_equil=1;

  int m=this->_m;
  int n=this->_n;
  
  T *d = _de;
  T *e = d+m;
   

  // Number of elements in matrix.
  size_t num_el = this->_m * this->_n;

  // Create bit-vector with signs of entries in A and then let A = f(A),
  // where f = |A| or f = |A|.^2.
  unsigned char *sign = 0;
  size_t num_sign_bytes = (num_el + 7) / 8;
  sign = new unsigned char[num_sign_bytes];
  ASSERT(sign != 0);
  size_t num_chars = num_el / 8;

  if(equillocal){

    //    fprintf(stderr,"Doing Equil\n"); fflush(stderr);
    
    // Fill sign bits, assigning each thread a multiple of 8 elements.
    if (kNormEquilibrate == kNorm2 || kNormEquilibrate == kNormFro) {
      SetSign(_data, sign, num_chars, SquareF<T>());
    } else {
      SetSign(_data, sign, num_chars, AbsF<T>());
    }

    // If numel(A) is not a multiple of 8, then we need to set the last couple
    // of sign bits too. 
    if (num_el > num_chars * 8) {
      if (kNormEquilibrate == kNorm2 || kNormEquilibrate == kNormFro) {
        SetSignSingle(_data + num_chars * 8, sign + num_chars,
                      num_el - num_chars * 8, SquareF<T>());
      } else {
        SetSignSingle(_data + num_chars * 8, sign + num_chars, 
                      num_el - num_chars * 8, AbsF<T>());
      }
    }
  }
  // Perform Sinkhorn-Knopp equilibration.
  SinkhornKnopp(this, d, e, equillocal);

  if(equillocal){
    // Transform A = sign(A) .* sqrt(A) if 2-norm equilibration was performed,
    // or A = sign(A) .* A if the 1-norm was equilibrated.
    if (kNormEquilibrate == kNorm2 || kNormEquilibrate == kNormFro) {
      UnSetSign(_data, sign, num_chars, SqrtF<T>());
    } else {
      UnSetSign(_data, sign, num_chars, IdentityF<T>());
    }

    // Deal with last few entries if num_el is not a multiple of 8.
    if (num_el > num_chars * 8) {
      if (kNormEquilibrate == kNorm2 || kNormEquilibrate == kNormFro) {
        UnSetSignSingle(_data + num_chars * 8, sign + num_chars, 
                        num_el - num_chars * 8, SqrtF<T>());
      } else {
        UnSetSignSingle(_data + num_chars * 8, sign + num_chars, 
                        num_el - num_chars * 8, IdentityF<T>());
      }
    }
  }
  
  // Compute D := sqrt(D), E := sqrt(E), if 2-norm was equilibrated.
  if (kNormEquilibrate == kNorm2 || kNormEquilibrate == kNormFro) {
    std::transform(d, d + this->_m, d, SqrtF<T>());
    std::transform(e, e + this->_n, e, SqrtF<T>());
  }

  // Compute A := D * A * E.
  MultDiag(d, e, this->_m, this->_n, _ord, _data);

  // Scale A to have norm of 1 (in the kNormNormalize norm).
  T normA = NormEst(kNormNormalize, *this);
  gsl::vector<T> a_vec = gsl::vector_view_array(_data, num_el);
  gsl::vector_scale(&a_vec, 1 / normA);

  // Scale d and e to account for normalization of A.
  gsl::vector<T> d_vec = gsl::vector_view_array<T>(d, this->_m);
  gsl::vector<T> e_vec = gsl::vector_view_array<T>(e, this->_n);
  gsl::vector_scale(&d_vec, 1 / std::sqrt(normA));
  gsl::vector_scale(&e_vec, 1 / std::sqrt(normA));

  DEBUG_PRINTF("norm A = %e, normd = %e, norme = %e\n", normA,
      gsl::blas_nrm2(&d_vec), gsl::blas_nrm2(&e_vec));

  delete [] sign;

  return 0;
}



template<typename T>
T getVar(size_t len, T *v, T mean) {
  double var = 0;
  for (size_t i = 0; i < len; ++i) {
    var += (v[i] - mean) * (v[i] - mean);
  }
  return static_cast<T>(var / (len - 1));
}
  

template <typename T>
int MatrixDense<T>::Stats(int intercept, T *min, T *max, T *mean, T *var, T *sd, T *skew, T *kurt, T &lambda_max0)
{
  int len=0;

  // Training mean and stddev
  len=this->_m;
  min[0]=*std::min_element(_datay, _datay+len);
  max[0]=*std::max_element(_datay, _datay+len);
  mean[0] = std::accumulate(_datay, _datay+len, T(0)) / len;
  var[0] = getVar(len,_datay, mean[0]);
  sd[0] = std::sqrt(var[0]);
  skew[0]=0.0; // not implemented
  kurt[0]=0.0; // not implemented

    // Training mean and stddev
  len=this->_mvalid;
  min[1]=*std::min_element(_datay, _datay+len);
  max[1]=*std::max_element(_datay, _datay+len);
  mean[1] = std::accumulate(_vdatay, _vdatay+len, T(0)) / len;
  var[1] = getVar(len,_vdatay, mean[1]);
  sd[1] = std::sqrt(var[1]);
  skew[1]=0.0; // not implemented
  kurt[1]=0.0; // not implemented

  size_t n=this->_n;
  size_t mTrain=this->_m;

  // TODO: compute on the GPU - inside of ElasticNetPtr
  // set lambda max 0 (i.e. base lambda_max)
  lambda_max0 = static_cast<T>(0);
  for (unsigned int j = 0; j < n-intercept; ++j) { //col
    T u = 0;
    T weights = static_cast<T>(1.0); ///mTrain); //TODO: Add per-obs weights
    for (unsigned int i = 0; i < mTrain; ++i) { //row
      u += weights * _data[i * n + j] * (_datay[i] - intercept*mean[0]);
    }
    lambda_max0 = static_cast<T>(std::max(lambda_max0, std::abs(u)));
  }
  
  

  return 0;
}

  

////////////////////////////////////////////////////////////////////////////////
/////////////////////// Equilibration Helpers //////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
namespace {

// Estimates norm of A. norm_type should either be kNorm2 or kNormFro.
template <typename T>
T NormEst(NormTypes norm_type, const MatrixDense<T>& A) {
  switch (norm_type) {
    case kNorm2: {
      return Norm2Est(&A);
    }
    case kNormFro: {
      const gsl::vector<T> a = gsl::vector_view_array(A.Data(),
          A.Rows() * A.Cols());
      return gsl::blas_nrm2(&a) /
          std::sqrt(static_cast<T>(std::min(A.Rows(), A.Cols())));
    }
    case kNorm1:
      // 1-norm normalization doens't make make sense since it treats rows and
      // columns differently.
    default:
      ASSERT(false);
      return static_cast<T>(0.);
  }
}

// Performs A := D * A * E for A in row major
template <typename T>
void MultRow(size_t m, size_t n, const T *d, const T *e, T *data) {
#ifdef _OPENMP
#pragma omp parallel for
#endif
  for (size_t t = 0; t < m * n; ++t)
    data[t] *= d[t / n] * e[t % n];
}

// Performs A := D * A * E for A in col major
template <typename T>
void MultCol(size_t m, size_t n, const T *d, const T *e, T *data) {
#ifdef _OPENMP
#pragma omp parallel for
#endif
  for (size_t t = 0; t < m * n; ++t)
    data[t] *= d[t % m] * e[t / m];
}

template <typename T>
void MultDiag(const T *d, const T *e, size_t m, size_t n,
              typename MatrixDense<T>::Ord ord, T *data) {
  if (ord == MatrixDense<T>::ROW) {
    MultRow(m, n, d, e, data);
  } else {
    MultCol(m, n, d, e, data);
  }
}


}  // namespace

// Explicit template instantiation.
#if !defined(POGS_DOUBLE) || POGS_DOUBLE==1
template class MatrixDense<double>;
#endif

#if !defined(POGS_SINGLE) || POGS_SINGLE==1
template class MatrixDense<float>;
#endif

}  // namespace pogs

