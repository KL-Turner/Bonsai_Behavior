using System;
using System.ComponentModel;
using System.Collections.Generic;
using System.Linq;
using System.Reactive.Linq;
using Bonsai;


[Combinator]
[Description("Polls an analog channel at regular intervals using a Timer node.")]
[WorkflowElementCategory(ElementCategory.Transform)]
public class SampleAnalogChannel
{
    // Main Process method
    public IObservable<float> Process(IObservable<long> Timer, IObservable<float> AnalogInput)
    {
        // Combine the Timer with the AnalogInput, starting with an initial false value to trigger CombineLatest
        return Timer
            .CombineLatest(AnalogInput.StartWith(0f).DistinctUntilChanged(), 
                           (time, value) => value);
    }
}