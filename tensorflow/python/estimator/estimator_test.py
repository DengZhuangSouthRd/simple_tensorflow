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
"""Tests for Estimator."""

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

from tensorflow.python.client import session
from tensorflow.python.estimator import estimator
from tensorflow.python.estimator import model_fn as model_fn_lib
from tensorflow.python.estimator import run_config
from tensorflow.python.framework import constant_op
from tensorflow.python.ops import control_flow_ops
from tensorflow.python.ops import state_ops
from tensorflow.python.ops import variables
from tensorflow.python.platform import test
from tensorflow.python.platform import tf_logging as logging
from tensorflow.python.training import saver
from tensorflow.python.training import session_run_hook
from tensorflow.python.training import training


def dummy_model_fn(features, labels, params):
  _, _, _ = features, labels, params


class EstimatorInheritanceConstraintTest(test.TestCase):
  """Tests that sub classes cannot override methods of Estimator."""

  def test_override_a_method(self):
    class _Estimator(estimator.Estimator):

      def __init__(self):
        super(_Estimator, self).__init__(model_fn=dummy_model_fn)

      def predict(self, input_fn, predict_keys=None, hooks=None):
        pass

    with self.assertRaisesRegexp(
        ValueError, 'cannot override members of Estimator.*predict'):
      _Estimator()

  def test_extension_of_api_is_ok(self):
    class _Estimator(estimator.Estimator):

      def __init__(self):
        super(_Estimator, self).__init__(model_fn=dummy_model_fn)

      def predict_proba(self, input_fn, predict_keys=None, hooks=None):
        pass

    _Estimator()


class EstimatorConstructorTest(test.TestCase):

  def test_config_must_be_a_run_config(self):
    with self.assertRaisesRegexp(ValueError, 'an instance of RunConfig'):
      estimator.Estimator(model_fn=None, config='NotARunConfig')

  def test_model_fn_must_be_provided(self):
    with self.assertRaisesRegexp(ValueError, 'model_fn.* must be'):
      estimator.Estimator(model_fn=None)

  def test_property_accessors(self):

    def model_fn(features, labels, params):
      _, _, _ = features, labels, params

    class FakeConfig(run_config.RunConfig):  # pylint: disable=g-wrong-blank-lines
      pass

    params = {'hidden_layers': [3, 4]}
    est = estimator.Estimator(
        model_fn=model_fn, model_dir='bla', config=FakeConfig(), params=params)
    self.assertTrue(isinstance(est.config, FakeConfig))
    self.assertEqual(params, est.params)
    self.assertEqual('bla', est.model_dir)

  def test_default_config(self):

    def model_fn(features, labels):
      _, _ = features, labels

    est = estimator.Estimator(model_fn=model_fn)
    self.assertTrue(isinstance(est.config, run_config.RunConfig))

  def test_default_model_dir(self):

    def model_fn(features, labels):
      _, _ = features, labels

    est = estimator.Estimator(model_fn=model_fn)
    self.assertTrue(est.model_dir is not None)

  def test_model_fn_args_must_include_features(self):

    def model_fn(x, labels):
      _, _ = x, labels

    with self.assertRaisesRegexp(ValueError, 'features'):
      estimator.Estimator(model_fn=model_fn)

  def test_model_fn_args_must_include_labels(self):

    def model_fn(features, y):
      _, _ = features, y

    with self.assertRaisesRegexp(ValueError, 'labels'):
      estimator.Estimator(model_fn=model_fn)

  def test_if_params_provided_then_model_fn_should_accept_it(self):

    def model_fn(features, labels):
      _, _ = features, labels

    estimator.Estimator(model_fn=model_fn)
    with self.assertRaisesRegexp(ValueError, 'params'):
      estimator.Estimator(model_fn=model_fn, params={'hidden_layers': 4})

  def test_not_known_model_fn_args(self):

    def model_fn(features, labels, something):
      _, _, _ = features, labels, something

    with self.assertRaisesRegexp(ValueError, 'something'):
      estimator.Estimator(model_fn=model_fn)

  def test_not_known_model_fn_args_handled_by_lambda(self):
    def model_fn(features, labels, something):
      _, _, _ = features, labels, something

    new_model_fn = lambda features, labels: model_fn(  # pylint: disable=g-long-lambda
        features, labels, 'something')
    estimator.Estimator(model_fn=new_model_fn)


def dummy_input_fn():
  return {'x': [[1], [1]]}, [[1], [1]]


def model_fn_global_step_incrementer(features, labels, mode):
  _, _ = features, labels
  global_step = training.get_global_step()
  return model_fn_lib.EstimatorSpec(
      mode,
      loss=constant_op.constant(1.),
      train_op=state_ops.assign_add(global_step, 1))


class EstimatorTrainTest(test.TestCase):

  def test_model_fn_must_return_estimator_spec(self):

    def model_fn(features, labels):
      _, _ = features, labels
      return 'NotGoodNotGood'

    est = estimator.Estimator(model_fn=model_fn)
    with self.assertRaisesRegexp(ValueError, 'EstimatorSpec'):
      est.train(dummy_input_fn, steps=1)

  def test_run_train_op_and_saves_at_the_end(self):
    est = estimator.Estimator(model_fn=model_fn_global_step_incrementer)
    est.train(dummy_input_fn, steps=5)
    self.assertEqual(
        5, estimator._load_global_step_from_checkpoint_dir(est.model_dir))

  def test_steps_and_saves_reloads(self):
    est = estimator.Estimator(model_fn=model_fn_global_step_incrementer)
    est.train(dummy_input_fn, steps=5)
    self.assertEqual(
        5, estimator._load_global_step_from_checkpoint_dir(est.model_dir))
    est.train(dummy_input_fn, steps=5)
    self.assertEqual(
        10, estimator._load_global_step_from_checkpoint_dir(est.model_dir))

  def test_max_step(self):
    est = estimator.Estimator(model_fn=model_fn_global_step_incrementer)
    est.train(dummy_input_fn, max_steps=5)
    self.assertEqual(
        5, estimator._load_global_step_from_checkpoint_dir(est.model_dir))
    est.train(dummy_input_fn, max_steps=5)
    self.assertEqual(
        5, estimator._load_global_step_from_checkpoint_dir(est.model_dir))

  def test_steps0_raises_error(self):
    est = estimator.Estimator(
        model_fn=_model_fn_with_eval_metric_ops)
    with self.assertRaisesRegexp(ValueError, 'Must specify steps >= 0'):
      est.train(dummy_input_fn, steps=0)

  def test_steps_negative_raises_error(self):
    est = estimator.Estimator(
        model_fn=_model_fn_with_eval_metric_ops)
    with self.assertRaisesRegexp(ValueError, 'Must specify steps >= 0'):
      est.train(dummy_input_fn, steps=-1)

  def test_max_steps0_raises_error(self):
    est = estimator.Estimator(
        model_fn=_model_fn_with_eval_metric_ops)
    with self.assertRaisesRegexp(ValueError, 'Must specify max_steps >= 0'):
      est.train(dummy_input_fn, max_steps=0)

  def test_max_steps_negative_raises_error(self):
    est = estimator.Estimator(
        model_fn=_model_fn_with_eval_metric_ops)
    with self.assertRaisesRegexp(ValueError, 'Must specify max_steps >= 0'):
      est.train(dummy_input_fn, max_steps=-1)

  def test_scaffold_is_used(self):
    self.is_init_fn_called = False

    def _init_fn(scaffold, sess):
      _, _ = scaffold, sess
      self.is_init_fn_called = True

    def _model_fn_scaffold(features, labels, mode):
      _, _ = features, labels
      return model_fn_lib.EstimatorSpec(
          mode=mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          scaffold=training.Scaffold(init_fn=_init_fn))

    est = estimator.Estimator(model_fn=_model_fn_scaffold)
    est.train(dummy_input_fn, steps=1)
    self.assertTrue(self.is_init_fn_called)

  def test_training_hooks_are_used(self):
    chief_hook = test.mock.MagicMock(
        wraps=training.SessionRunHook(), spec=training.SessionRunHook)
    hook = test.mock.MagicMock(
        wraps=training.SessionRunHook(), spec=training.SessionRunHook)

    def _model_fn_hooks(features, labels, mode):
      _, _ = features, labels
      return model_fn_lib.EstimatorSpec(
          mode=mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          training_chief_hooks=[chief_hook],
          training_hooks=[hook])

    est = estimator.Estimator(model_fn=_model_fn_hooks)
    self.assertFalse(chief_hook.begin.called)
    self.assertFalse(hook.begin.called)
    est.train(dummy_input_fn, steps=1)
    self.assertTrue(chief_hook.begin.called)
    self.assertTrue(hook.begin.called)

  def test_chief_only_hook_should_not_be_called_on_non_chief(self):
    chief_hook = test.mock.MagicMock(
        wraps=training.SessionRunHook(), spec=training.SessionRunHook)
    hook = test.mock.MagicMock(
        wraps=training.SessionRunHook(), spec=training.SessionRunHook)

    def _model_fn_hooks(features, labels, mode):
      _, _ = features, labels
      return model_fn_lib.EstimatorSpec(
          mode=mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          training_chief_hooks=[chief_hook],
          training_hooks=[hook])

    class NonChiefRunConfig(run_config.RunConfig):  # pylint: disable=g-wrong-blank-lines
      @property
      def is_chief(self):  # pylint: disable=g-wrong-blank-lines
        return False

    # Mocking the SessionManager.wait_for_session, so that worker doesn't wait
    # for chief.
    def get_initialized_session(*args, **kwargs):
      scaffold = training.Scaffold().finalize()
      sess = session.Session(*args, **kwargs)
      sess.run(scaffold.init_op)
      return sess

    with test.mock.patch.object(
        training.SessionManager,
        'wait_for_session',
        side_effect=get_initialized_session):
      est = estimator.Estimator(
          model_fn=_model_fn_hooks, config=NonChiefRunConfig())
      self.assertFalse(chief_hook.begin.called)
      self.assertFalse(hook.begin.called)
      est.train(dummy_input_fn, steps=1)
      self.assertFalse(chief_hook.begin.called)
      self.assertTrue(hook.begin.called)

  def test_features_labels_mode(self):
    given_features = {'test-features': [[1], [1]]}
    given_labels = {'test-labels': [[1], [1]]}

    def _input_fn():
      return given_features, given_labels

    def _model_fn(features, labels, mode):
      self.features, self.labels, self.mode = features, labels, mode
      return model_fn_lib.EstimatorSpec(
          mode=mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          predictions=constant_op.constant(0.))

    est = estimator.Estimator(model_fn=_model_fn)
    est.train(_input_fn, steps=1)
    self.assertEqual(given_features, self.features)
    self.assertEqual(given_labels, self.labels)
    self.assertEqual(model_fn_lib.ModeKeys.TRAIN, self.mode)


def _model_fn_with_eval_metric_ops(features, labels, mode, params):
  _, _ = features, labels
  metric_name = params.get('metric_name') or 'metric'
  metric_value = params.get('metric_value') or 2.
  global_step = training.get_global_step()
  loss = constant_op.constant(1.)
  metric_update_op = loss.op
  metric_tensor = control_flow_ops.with_dependencies(
      [metric_update_op], constant_op.constant(metric_value))
  return model_fn_lib.EstimatorSpec(
      mode,
      loss=loss,
      predictions={'predictions': constant_op.constant(1.)},
      train_op=state_ops.assign_add(global_step, 1),
      eval_metric_ops={metric_name: (metric_tensor, metric_update_op)})


class _StepCounterHook(session_run_hook.SessionRunHook):
  """Hooks that counts the number of times it is called."""

  def __init__(self):
    self._steps = 0

  def before_run(self, run_context):
    del run_context
    self._steps += 1

  @property
  def steps(self):
    return self._steps


class EstimatorEvaluateTest(test.TestCase):

  def test_model_fn_must_return_estimator_spec(self):
    def _model_fn(features, labels, mode):
      _, _ = features, labels
      if mode == model_fn_lib.ModeKeys.EVAL:
        return 'NotGoodNotGood'
      return model_fn_lib.EstimatorSpec(
          mode,
          loss=constant_op.constant(1.),
          train_op=control_flow_ops.no_op())

    est = estimator.Estimator(model_fn=_model_fn)
    est.train(dummy_input_fn, steps=1)
    with self.assertRaisesRegexp(
        ValueError, 'model_fn should return an EstimatorSpec'):
      est.evaluate(dummy_input_fn, steps=1)

  def test_no_trained_model(self):
    est = estimator.Estimator(model_fn=_model_fn_with_eval_metric_ops)
    with self.assertRaisesRegexp(
        ValueError, 'Could not find trained model in model_dir'):
      est.evaluate(dummy_input_fn, steps=1)

  def test_scores(self):
    est = estimator.Estimator(
        model_fn=_model_fn_with_eval_metric_ops,
        params={
            'metric_name': 'metric',
            'metric_value': 2.})
    est.train(dummy_input_fn, steps=5)
    scores = est.evaluate(dummy_input_fn, steps=1)
    self.assertDictEqual(
        {'metric': 2.,
         'global_step': 5},
        scores)

  def test_steps0_raises_error(self):
    est = estimator.Estimator(
        model_fn=_model_fn_with_eval_metric_ops)
    est.train(dummy_input_fn, steps=5)
    with self.assertRaisesRegexp(ValueError, 'Must specify steps >= 0'):
      est.evaluate(dummy_input_fn, steps=0)

  def test_steps_negative_raises_error(self):
    est = estimator.Estimator(
        model_fn=_model_fn_with_eval_metric_ops)
    est.train(dummy_input_fn, steps=5)
    with self.assertRaisesRegexp(ValueError, 'Must specify steps >= 0'):
      est.evaluate(dummy_input_fn, steps=-1)

  def test_global_step_metric_raises_error(self):
    est = estimator.Estimator(
        model_fn=_model_fn_with_eval_metric_ops,
        params={
            'metric_name': 'global_step',
            'metric_value': 2.})
    est.train(dummy_input_fn, steps=5)
    with self.assertRaisesRegexp(
        ValueError, 'Metric with name `global_step` is not allowed'):
      est.evaluate(dummy_input_fn, steps=1)

  def test_hooks_are_used(self):
    step_counter_hook = _StepCounterHook()

    est = estimator.Estimator(model_fn=_model_fn_with_eval_metric_ops)
    est.train(dummy_input_fn, steps=1)
    est.evaluate(dummy_input_fn, steps=5, hooks=[step_counter_hook])
    self.assertEqual(5, step_counter_hook.steps)

  def test_evaluate_from_checkpoint(self):
    params = {
        'metric_name': 'metric',
        'metric_value': 2.}
    est1 = estimator.Estimator(
        model_fn=_model_fn_with_eval_metric_ops,
        params=params)
    est1.train(dummy_input_fn, steps=5)
    est2 = estimator.Estimator(
        model_fn=_model_fn_with_eval_metric_ops,
        params=params)
    scores = est2.evaluate(
        dummy_input_fn,
        steps=1,
        checkpoint_path=saver.latest_checkpoint(est1.model_dir))
    self.assertDictEqual(
        {'metric': 2.,
         'global_step': 5},
        scores)

  def test_scaffold_is_used(self):

    def _model_fn_scaffold(features, labels, mode):
      _, _ = features, labels
      variables.Variable(1., 'weight')
      real_saver = saver.Saver()
      self.mock_saver = test.mock.Mock(
          wraps=real_saver, saver_def=real_saver.saver_def)
      return model_fn_lib.EstimatorSpec(
          mode=mode,
          predictions=constant_op.constant([[1.]]),
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          scaffold=training.Scaffold(saver=self.mock_saver))

    est = estimator.Estimator(model_fn=_model_fn_scaffold)
    est.train(dummy_input_fn, steps=1)
    est.evaluate(dummy_input_fn, steps=1)
    self.assertTrue(self.mock_saver.restore.called)

  def test_features_labels_mode(self):
    given_features = {'test-features': [[1], [1]]}
    given_labels = {'test-labels': [[1], [1]]}

    def _input_fn():
      return given_features, given_labels

    def _model_fn(features, labels, mode):
      self.features, self.labels, self.mode = features, labels, mode
      return model_fn_lib.EstimatorSpec(
          mode=mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          predictions=constant_op.constant(0.))

    est = estimator.Estimator(model_fn=_model_fn)
    est.train(_input_fn, steps=1)
    est.evaluate(_input_fn, steps=1)
    self.assertEqual(given_features, self.features)
    self.assertEqual(given_labels, self.labels)
    self.assertEqual(model_fn_lib.ModeKeys.EVAL, self.mode)


class EstimatorPredictTest(test.TestCase):

  def test_no_trained_model(self):
    est = estimator.Estimator(model_fn=model_fn_global_step_incrementer)
    with self.assertRaisesRegexp(ValueError,
                                 'Could not find trained model in model_dir'):
      next(est.predict(dummy_input_fn))

  def test_tensor_predictions(self):

    def _model_fn(features, labels, mode):
      _, _ = features, labels
      return model_fn_lib.EstimatorSpec(
          mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          predictions=constant_op.constant([[10.]]))

    est = estimator.Estimator(model_fn=_model_fn)
    est.train(dummy_input_fn, steps=1)
    self.assertEqual(10., next(est.predict(dummy_input_fn)))

  def test_warn_if_no_queue_runner(self):

    def _model_fn(features, labels, mode):
      _, _ = features, labels
      return model_fn_lib.EstimatorSpec(
          mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          predictions=constant_op.constant([[10.]]))

    est = estimator.Estimator(model_fn=_model_fn)
    est.train(dummy_input_fn, steps=1)
    with test.mock.patch.object(logging, 'warning') as mock_log:
      next(est.predict(dummy_input_fn))
      self.assertRegexpMatches(
          str(mock_log.call_args),
          'Input graph does not contain a QueueRunner.')

  def test_input_fn_can_return_just_features(self):

    def _model_fn(features, labels, mode):
      _, _ = features, labels
      return model_fn_lib.EstimatorSpec(
          mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          predictions=constant_op.constant([[10.]]))

    est = estimator.Estimator(model_fn=_model_fn)
    est.train(dummy_input_fn, steps=1)

    def _only_features():
      return {'x': constant_op.constant([[0.]])}

    self.assertEqual([10.], next(est.predict(_only_features)))

  def test_batch_size_mismatch(self):

    def _model_fn(features, labels, mode):
      _, _ = features, labels
      return model_fn_lib.EstimatorSpec(
          mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          predictions={
              'y1': constant_op.constant([[10.]]),
              'y2': constant_op.constant([[12.], [13]])
          })

    est = estimator.Estimator(model_fn=_model_fn)
    est.train(dummy_input_fn, steps=1)
    with self.assertRaisesRegexp(ValueError,
                                 'Batch length of predictions should be same'):
      next(est.predict(dummy_input_fn))

  def test_predict_keys_defined_for_tensor(self):

    def _model_fn(features, labels, mode):
      _, _ = features, labels
      return model_fn_lib.EstimatorSpec(
          mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          predictions=constant_op.constant([[10.]]))

    est = estimator.Estimator(model_fn=_model_fn)
    est.train(dummy_input_fn, steps=1)
    with self.assertRaisesRegexp(
        ValueError,
        'predict_keys argument is not valid in case of non-dict predictions'):
      next(est.predict(dummy_input_fn, predict_keys=['y']))

  def test_predict_keys_does_not_exists(self):

    def _model_fn(features, labels, mode):
      _, _ = features, labels
      return model_fn_lib.EstimatorSpec(
          mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          predictions={
              'y1': constant_op.constant([[10.]]),
              'y2': constant_op.constant([[12.]])
          })

    est = estimator.Estimator(model_fn=_model_fn)
    est.train(dummy_input_fn, steps=1)
    with self.assertRaisesRegexp(ValueError,
                                 'Expected to run at least one output from'):
      next(est.predict(dummy_input_fn, predict_keys=['y3']))

  def test_return_given_predict_keys(self):

    def _model_fn(features, labels, mode):
      _, _ = features, labels
      return model_fn_lib.EstimatorSpec(
          mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          predictions={
              'y1': constant_op.constant([[10.]]),
              'y2': constant_op.constant([[12.]])
          })

    est = estimator.Estimator(model_fn=_model_fn)
    est.train(dummy_input_fn, steps=1)
    results = next(est.predict(dummy_input_fn, predict_keys=['y1']))
    self.assertIn('y1', results)
    self.assertNotIn('y2', results)

  def test_yield_rows_of_tensor(self):

    def _model_fn(features, labels, mode):
      _, _ = features, labels
      return model_fn_lib.EstimatorSpec(
          mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          predictions=constant_op.constant([[10.], [12.]]))

    est = estimator.Estimator(model_fn=_model_fn)
    est.train(dummy_input_fn, steps=1)
    results = est.predict(dummy_input_fn)
    self.assertEqual([10.], next(results))
    self.assertEqual([12.], next(results))

  def test_yield_rows_of_dict(self):

    def _model_fn(features, labels, mode):
      _, _ = features, labels
      return model_fn_lib.EstimatorSpec(
          mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          predictions={
              'y1': constant_op.constant([[10.], [12]]),
              'y2': constant_op.constant([[0.], [2.]])
          })

    est = estimator.Estimator(model_fn=_model_fn)
    est.train(dummy_input_fn, steps=1)
    results = est.predict(dummy_input_fn)
    self.assertDictEqual({'y1': [10.], 'y2': [0.]}, next(results))
    self.assertDictEqual({'y1': [12.], 'y2': [2.]}, next(results))

  def test_hooks_are_used(self):

    def _model_fn(features, labels, mode):
      _, _ = features, labels
      return model_fn_lib.EstimatorSpec(
          mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          predictions=constant_op.constant([[10.], [12.]]))

    step_counter_hook = _StepCounterHook()
    est = estimator.Estimator(model_fn=_model_fn)
    est.train(dummy_input_fn, steps=1)
    results = est.predict(dummy_input_fn, hooks=[step_counter_hook])
    self.assertEqual(0, step_counter_hook.steps)  # not called yet
    next(results)
    self.assertEqual(1, step_counter_hook.steps)  # first call
    next(results)
    self.assertEqual(1, step_counter_hook.steps)  # it's in same batch
    next(results)
    self.assertEqual(2, step_counter_hook.steps)  # next batch

  def test_predict_from_old_model_dir(self):

    def _model_fn(features, labels, mode):
      _, _ = features, labels
      v = variables.Variable([[16.]], 'weight')
      prediction = v * 2
      return model_fn_lib.EstimatorSpec(
          mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          predictions=prediction)

    est1 = estimator.Estimator(model_fn=_model_fn)
    est1.train(dummy_input_fn, steps=1)
    est2 = estimator.Estimator(model_fn=_model_fn, model_dir=est1.model_dir)
    self.assertEqual([32.], next(est2.predict(dummy_input_fn)))

  def test_scaffold_is_used(self):

    def _model_fn_scaffold(features, labels, mode):
      _, _ = features, labels
      variables.Variable(1., 'weight')
      real_saver = saver.Saver()
      self.mock_saver = test.mock.Mock(
          wraps=real_saver, saver_def=real_saver.saver_def)
      return model_fn_lib.EstimatorSpec(
          mode=mode,
          predictions=constant_op.constant([[1.]]),
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          scaffold=training.Scaffold(saver=self.mock_saver))

    est = estimator.Estimator(model_fn=_model_fn_scaffold)
    est.train(dummy_input_fn, steps=1)
    next(est.predict(dummy_input_fn))
    self.assertTrue(self.mock_saver.restore.called)

  def test_features_labels_mode(self):
    given_features = {'test-features': [[1], [1]]}
    given_labels = {'test-labels': [[1], [1]]}

    def _input_fn():
      return given_features, given_labels

    def _model_fn(features, labels, mode):
      self.features, self.labels, self.mode = features, labels, mode
      return model_fn_lib.EstimatorSpec(
          mode=mode,
          loss=constant_op.constant(0.),
          train_op=constant_op.constant(0.),
          predictions=constant_op.constant([[0.]]))

    est = estimator.Estimator(model_fn=_model_fn)
    est.train(_input_fn, steps=1)
    next(est.predict(_input_fn))
    self.assertEqual(given_features, self.features)
    self.assertIsNone(self.labels)
    self.assertEqual(model_fn_lib.ModeKeys.PREDICT, self.mode)


if __name__ == '__main__':
  test.main()
