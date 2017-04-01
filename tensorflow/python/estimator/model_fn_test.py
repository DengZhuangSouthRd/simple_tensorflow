# Copyright 2016 The TensorFlow Authors. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# ==============================================================================

"""Tests for model_fn.py."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.estimator import model_fn
from tensorflow.python.framework import constant_op
from tensorflow.python.framework import ops
from tensorflow.python.framework import sparse_tensor
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.platform import test
from tensorflow.python.saved_model import signature_constants
from tensorflow.python.training import monitored_session
from tensorflow.python.training import session_run_hook


class _FakeHook(session_run_hook.SessionRunHook):
  """Fake implementation of `SessionRunHook`."""


class _InvalidHook(object):
  """Invalid hook (not a subclass of `SessionRunHook`)."""


class _InvalidScaffold(object):
  """Invalid scaffold (not a subclass of `Scaffold`)."""


class EstimatorSpecTrainTest(test.TestCase):
  """Tests EstimatorSpec in train mode."""

  def testRequiredArgumentsSet(self):
    """Tests that no errors are raised when all required arguments are set."""
    with ops.Graph().as_default(), self.test_session():
      model_fn.EstimatorSpec(
          mode=model_fn.ModeKeys.TRAIN,
          loss=constant_op.constant(1.),
          train_op=control_flow_ops.no_op())

  def testAllArgumentsSet(self):
    """Tests that no errors are raised when all arguments are set."""
    with ops.Graph().as_default(), self.test_session():
      loss = constant_op.constant(1.)
      predictions = {'loss': loss}
      model_fn.EstimatorSpec(
          mode=model_fn.ModeKeys.TRAIN,
          predictions=predictions,
          loss=loss,
          train_op=control_flow_ops.no_op(),
          eval_metric_ops={'loss': (control_flow_ops.no_op(), loss)},
          export_outputs={
              'head_name': (signature_constants.CLASSIFY_METHOD_NAME,
                            predictions)
          },
          training_chief_hooks=[_FakeHook()],
          training_hooks=[_FakeHook()],
          scaffold=monitored_session.Scaffold())

  def testLossNumber(self):
    """Tests that error is raised when loss is a number (not Tensor)."""
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(TypeError, 'loss must be Tensor'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.TRAIN,
            loss=1.,
            train_op=control_flow_ops.no_op())

  def testLoss1DTensor(self):
    """Tests that no errors are raised when loss is 1D tensor."""
    with ops.Graph().as_default(), self.test_session():
      model_fn.EstimatorSpec(
          mode=model_fn.ModeKeys.TRAIN,
          loss=constant_op.constant([1.]),
          train_op=control_flow_ops.no_op())

  def testLossMissing(self):
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(ValueError, 'Missing loss'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.TRAIN, train_op=control_flow_ops.no_op())

  def testLossNotScalar(self):
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(ValueError, 'Loss must be scalar'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.TRAIN,
            loss=constant_op.constant([1., 2.]),
            train_op=control_flow_ops.no_op())

  def testLossSparseTensor(self):
    with ops.Graph().as_default(), self.test_session():
      loss = sparse_tensor.SparseTensor(
          indices=[[0]],
          values=[0.],
          dense_shape=[1])
      with self.assertRaisesRegexp(TypeError, 'loss must be Tensor'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.TRAIN,
            loss=loss,
            train_op=control_flow_ops.no_op())

  def testLossFromDifferentGraph(self):
    with ops.Graph().as_default():
      loss = constant_op.constant(1.)
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(
          ValueError, 'must be from the default graph'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.TRAIN,
            loss=loss,
            train_op=control_flow_ops.no_op())

  def testTrainOpMissing(self):
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(ValueError, 'Missing train_op'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.TRAIN, loss=constant_op.constant(1.))

  def testTrainOpNotOperationAndTensor(self):
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(TypeError,
                                   'train_op must be Operation or Tensor'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.TRAIN,
            loss=constant_op.constant(1.),
            train_op='Not an Operation or Tensor')

  def testTrainOpFromDifferentGraph(self):
    with ops.Graph().as_default():
      train_op = control_flow_ops.no_op()
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(
          ValueError, 'must be from the default graph'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.TRAIN,
            loss=constant_op.constant(1.),
            train_op=train_op)

  def testTrainingChiefHookInvalid(self):
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(
          TypeError, 'All hooks must be SessionRunHook instances'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.TRAIN,
            loss=constant_op.constant(1.),
            train_op=control_flow_ops.no_op(),
            training_chief_hooks=[_InvalidHook()])

  def testTrainingHookInvalid(self):
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(
          TypeError, 'All hooks must be SessionRunHook instances'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.TRAIN,
            loss=constant_op.constant(1.),
            train_op=control_flow_ops.no_op(),
            training_hooks=[_InvalidHook()])

  def testScaffoldInvalid(self):
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(
          TypeError, r'scaffold must be tf\.train\.Scaffold'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.TRAIN,
            loss=constant_op.constant(1.),
            train_op=control_flow_ops.no_op(),
            scaffold=_InvalidScaffold())

  def testReturnDefaultScaffold(self):
    with ops.Graph().as_default(), self.test_session():
      estimator_spec = model_fn.EstimatorSpec(
          mode=model_fn.ModeKeys.TRAIN,
          loss=constant_op.constant(1.),
          train_op=control_flow_ops.no_op())
      self.assertIsNotNone(estimator_spec.scaffold)


class EstimatorSpecEvalTest(test.TestCase):
  """Tests EstimatorSpec in eval mode."""

  def testRequiredArgumentsSet(self):
    """Tests that no errors are raised when all required arguments are set."""
    with ops.Graph().as_default(), self.test_session():
      loss = constant_op.constant(1.)
      model_fn.EstimatorSpec(
          mode=model_fn.ModeKeys.EVAL,
          predictions={'loss': loss},
          loss=loss)

  def testAllArgumentsSet(self):
    """Tests that no errors are raised when all arguments are set."""
    with ops.Graph().as_default(), self.test_session():
      loss = constant_op.constant(1.)
      predictions = {'loss': loss}
      model_fn.EstimatorSpec(
          mode=model_fn.ModeKeys.EVAL,
          predictions=predictions,
          loss=loss,
          train_op=control_flow_ops.no_op(),
          eval_metric_ops={'loss': (control_flow_ops.no_op(), loss)},
          export_outputs={
              'head_name': (signature_constants.CLASSIFY_METHOD_NAME,
                            predictions)
          },
          training_chief_hooks=[_FakeHook()],
          training_hooks=[_FakeHook()],
          scaffold=monitored_session.Scaffold())

  def testLoss1DTensor(self):
    """Tests that no errors are raised when loss is 1D tensor."""
    with ops.Graph().as_default(), self.test_session():
      loss = constant_op.constant([1.])
      model_fn.EstimatorSpec(
          mode=model_fn.ModeKeys.EVAL,
          predictions={'loss': loss},
          loss=loss)

  def testLossNumber(self):
    """Tests that error is raised when loss is a number (not Tensor)."""
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(TypeError, 'loss must be Tensor'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.EVAL,
            predictions={'loss': constant_op.constant(1.)},
            loss=1.)

  def testLossMissing(self):
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(ValueError, 'Missing loss'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.EVAL,
            predictions={'loss': constant_op.constant(1.)})

  def testLossNotScalar(self):
    with ops.Graph().as_default(), self.test_session():
      loss = constant_op.constant([1., 2.])
      with self.assertRaisesRegexp(ValueError, 'Loss must be scalar'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.EVAL,
            predictions={'loss': loss},
            loss=loss)

  def testLossSparseTensor(self):
    with ops.Graph().as_default(), self.test_session():
      loss = sparse_tensor.SparseTensor(
          indices=[[0]],
          values=[0.],
          dense_shape=[1])
      with self.assertRaisesRegexp(
          TypeError, 'loss must be Tensor'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.EVAL,
            predictions={'prediction': constant_op.constant(1.)},
            loss=loss)

  def testLossFromDifferentGraph(self):
    with ops.Graph().as_default():
      loss = constant_op.constant(1.)
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(
          ValueError, 'must be from the default graph'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.EVAL,
            predictions={'prediction': constant_op.constant(1.)},
            loss=loss)

  def testPredictionsMissingIsOkay(self):
    with ops.Graph().as_default(), self.test_session():
      model_fn.EstimatorSpec(
          mode=model_fn.ModeKeys.EVAL, loss=constant_op.constant(1.))

  def testPredictionsTensor(self):
    """Tests that no error is raised when predictions is Tensor (not dict)."""
    with ops.Graph().as_default(), self.test_session():
      loss = constant_op.constant(1.)
      model_fn.EstimatorSpec(
          mode=model_fn.ModeKeys.EVAL,
          predictions=loss,
          loss=loss)

  def testPredictionsNumber(self):
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(
          TypeError, r'predictions\[number\] must be Tensor'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.EVAL,
            predictions={'number': 1.},
            loss=constant_op.constant(1.))

  def testPredictionsSparseTensor(self):
    with ops.Graph().as_default(), self.test_session():
      predictions = {
          'sparse': sparse_tensor.SparseTensor(
              indices=[[0]],
              values=[0.],
              dense_shape=[1])}
      with self.assertRaisesRegexp(
          TypeError, r'predictions\[sparse\] must be Tensor'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.EVAL,
            predictions=predictions,
            loss=constant_op.constant(1.))

  def testPredictionsFromDifferentGraph(self):
    with ops.Graph().as_default():
      predictions = {'loss': constant_op.constant(1.)}
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(
          ValueError, 'must be from the default graph'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.EVAL,
            predictions=predictions,
            loss=constant_op.constant(1.))

  def testEvalMetricOpsNoDict(self):
    with ops.Graph().as_default(), self.test_session():
      loss = constant_op.constant(1.)
      with self.assertRaisesRegexp(
          TypeError, 'eval_metric_ops must be a dict'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.EVAL,
            predictions={'loss': loss},
            loss=loss,
            eval_metric_ops=loss)

  def testEvalMetricOpsNoTuple(self):
    with ops.Graph().as_default(), self.test_session():
      loss = constant_op.constant(1.)
      with self.assertRaisesRegexp(
          TypeError,
          (r'Values of eval_metric_ops must be \(metric_tensor, update_op\) '
           'tuples')):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.EVAL,
            predictions={'loss': loss},
            loss=loss,
            eval_metric_ops={'loss': loss})

  def testEvalMetricOpsNoTensorOrOperation(self):
    with ops.Graph().as_default(), self.test_session():
      loss = constant_op.constant(1.)
      with self.assertRaisesRegexp(TypeError, 'must be Operation or Tensor'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.EVAL,
            predictions={'loss': loss},
            loss=loss,
            eval_metric_ops={'loss': ('NonTensor', loss)})

  def testEvalMetricOpsFromDifferentGraph(self):
    with ops.Graph().as_default():
      eval_metric_ops = {
          'loss': (control_flow_ops.no_op(), constant_op.constant(1.))}
    with ops.Graph().as_default(), self.test_session():
      loss = constant_op.constant(1.)
      with self.assertRaisesRegexp(
          ValueError, 'must be from the default graph'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.EVAL,
            predictions={'loss': loss},
            loss=loss,
            eval_metric_ops=eval_metric_ops)


class EstimatorSpecInferTest(test.TestCase):
  """Tests EstimatorSpec in infer mode."""

  def testRequiredArgumentsSet(self):
    """Tests that no errors are raised when all required arguments are set."""
    with ops.Graph().as_default(), self.test_session():
      model_fn.EstimatorSpec(
          mode=model_fn.ModeKeys.PREDICT,
          predictions={'loss': constant_op.constant(1.)})

  def testAllArgumentsSet(self):
    """Tests that no errors are raised when all arguments are set."""
    with ops.Graph().as_default(), self.test_session():
      loss = constant_op.constant(1.)
      predictions = {'loss': loss}
      model_fn.EstimatorSpec(
          mode=model_fn.ModeKeys.PREDICT,
          predictions=predictions,
          loss=loss,
          train_op=control_flow_ops.no_op(),
          eval_metric_ops={'loss': (control_flow_ops.no_op(), loss)},
          export_outputs={
              'head_name': (signature_constants.CLASSIFY_METHOD_NAME,
                            predictions)
          },
          training_chief_hooks=[_FakeHook()],
          training_hooks=[_FakeHook()],
          scaffold=monitored_session.Scaffold())

  def testPredictionsMissing(self):
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(ValueError, 'Missing predictions'):
        model_fn.EstimatorSpec(mode=model_fn.ModeKeys.PREDICT)

  def testPredictionsTensor(self):
    """Tests that no error is raised when predictions is Tensor (not dict)."""
    with ops.Graph().as_default(), self.test_session():
      model_fn.EstimatorSpec(
          mode=model_fn.ModeKeys.PREDICT, predictions=constant_op.constant(1.))

  def testPredictionsNumber(self):
    with ops.Graph().as_default(), self.test_session():
      with self.assertRaisesRegexp(
          TypeError, r'predictions\[number\] must be Tensor'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.PREDICT, predictions={'number': 1.})

  def testPredictionsSparseTensor(self):
    with ops.Graph().as_default(), self.test_session():
      predictions = {
          'sparse': sparse_tensor.SparseTensor(
              indices=[[0]],
              values=[0.],
              dense_shape=[1])}
      with self.assertRaisesRegexp(
          TypeError, r'predictions\[sparse\] must be Tensor'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.PREDICT, predictions=predictions)

  def testExportOutputsNoDict(self):
    with ops.Graph().as_default(), self.test_session():
      predictions = {'loss': constant_op.constant(1.)}
      with self.assertRaisesRegexp(
          TypeError, 'export_outputs must be dict'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.PREDICT,
            predictions=predictions,
            export_outputs=(signature_constants.CLASSIFY_METHOD_NAME,
                            predictions))

  def testExportOutputsValueNotTuple(self):
    with ops.Graph().as_default(), self.test_session():
      predictions = {'loss': constant_op.constant(1.)}
      with self.assertRaisesRegexp(
          TypeError, 'Values in export_outputs must be 2-tuple'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.PREDICT,
            predictions=predictions,
            export_outputs={'head_name': predictions})

  def testExportOutputsValue1Tuple(self):
    with ops.Graph().as_default(), self.test_session():
      predictions = {'loss': constant_op.constant(1.)}
      with self.assertRaisesRegexp(
          TypeError, 'Values in export_outputs must be 2-tuple'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.PREDICT,
            predictions=predictions,
            export_outputs={'head_name': (predictions,)})

  def testExportOutputsInvalidMethodName(self):
    with ops.Graph().as_default(), self.test_session():
      predictions = {'loss': constant_op.constant(1.)}
      with self.assertRaisesRegexp(
          ValueError, 'Invalid signature_method_name in export_outputs'):
        model_fn.EstimatorSpec(
            mode=model_fn.ModeKeys.PREDICT,
            predictions=predictions,
            export_outputs={'head_name': ('invalid/method/name', predictions)})


if __name__ == '__main__':
  test.main()
